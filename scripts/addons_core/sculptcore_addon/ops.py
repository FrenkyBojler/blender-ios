# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Engine-backed edit operators (P10 class-2 wrappers): whole-mesh operations
the vanilla sculpt operators do on the SculptSession, reimplemented over the
engine's attribute columns. Each op writes the column directly (no meshlog
entry) and records an attribute-snapshot undo step via undo.push_attr.

Multires sessions are excluded for now: their mask lives in per-grid
channels (A4), not the top-level column these ops write.
"""

import bpy

from . import convert, engine, multires, undo


def _session(context, allow_multires=False):
    ob = context.active_object
    if (ob is None or ob.mode != 'CUSTOM'
            or ob.custom_mode != "sculptcore.sculpt"):
        return None
    session = engine.sessions.get(ob.name)
    if session is None:
        return None
    if session.multires_ptr and not allow_multires:
        return None
    return session


class SCULPTCORE_OT_mask_flood_fill(bpy.types.Operator):
    """Fill the whole sculpt mask with a value"""
    bl_idname = "sculptcore.mask_flood_fill"
    bl_label = "Mask Flood Fill"
    # No 'UNDO': the op pushes its own attribute-snapshot step (undo.py).
    bl_options = {'REGISTER'}

    mode: bpy.props.EnumProperty(
        name="Mode",
        items=(
            ('VALUE', "Value", "Set the mask to the given value"),
            ('INVERT', "Invert", "Invert the mask"),
        ),
        default='VALUE',
    )
    value: bpy.props.FloatProperty(
        name="Value", default=0.0, min=0.0, max=1.0,
        description="Mask level to fill with",
    )

    @classmethod
    def poll(cls, context):
        return _session(context) is not None

    def execute(self, context):
        import numpy as np

        session = _session(context)
        lib = engine.capi().lib
        verts_num = convert.mesh_vert_num(session.mesh_ptr)

        before = np.zeros(verts_num, dtype=np.float32)
        lib.Mesh_readVertFloatAttr(session.mesh_ptr, convert._SC_MASK, before)

        if self.mode == 'INVERT':
            after = np.ascontiguousarray(1.0 - before)
        else:
            after = np.full(verts_num, self.value, dtype=np.float32)

        if not lib.Mesh_writeVertFloatAttr(session.mesh_ptr, convert._SC_MASK, after):
            self.report({'ERROR'}, "Engine rejected the mask write")
            return {'CANCELLED'}

        ob = context.active_object
        undo.push_attr(context, ob, session, "Mask Flood Fill", 'VERT_F32',
                       convert._SC_MASK, before.tobytes(), after.tobytes())
        # Write-back to the Mesh stays deferred to the mode flush, matching
        # the stroke policy; the engine state is authoritative meanwhile.
        undo._tag_view3d_redraw(context)
        return {'FINISHED'}


def _vert_adjacency(mesh_ptr, verts_num):
    """Unique undirected edges (as two aligned index arrays, both
    directions) from the engine's live topology."""
    import numpy as np

    corner_verts, face_offsets = convert.mesh_topo_arrays(mesh_ptr)
    # Each corner pairs with the next corner in its face (wrapping).
    nxt = np.arange(1, len(corner_verts) + 1, dtype=np.int64)
    nxt[face_offsets[1:] - 1] = face_offsets[:-1]
    a = corner_verts.astype(np.int64)
    b = corner_verts[nxt].astype(np.int64)
    lo, hi = np.minimum(a, b), np.maximum(a, b)
    unique = np.unique(lo * verts_num + hi)
    lo, hi = unique // verts_num, unique % verts_num
    return np.concatenate([lo, hi]), np.concatenate([hi, lo])


class SCULPTCORE_OT_mask_filter(bpy.types.Operator):
    """Apply a filter to the sculpt mask"""
    bl_idname = "sculptcore.mask_filter"
    bl_label = "Mask Filter"
    # No 'UNDO': attribute-snapshot step, like mask_flood_fill.
    bl_options = {'REGISTER'}

    filter_type: bpy.props.EnumProperty(
        name="Type",
        items=(
            ('SMOOTH', "Smooth Mask", "Smooth the mask"),
            ('SHARPEN', "Sharpen Mask", "Sharpen the mask"),
            ('GROW', "Grow Mask", "Grow the mask"),
            ('SHRINK', "Shrink Mask", "Shrink the mask"),
            ('CONTRAST_INCREASE', "Increase Contrast", "Increase the mask contrast"),
            ('CONTRAST_DECREASE', "Decrease Contrast", "Decrease the mask contrast"),
        ),
        default='SMOOTH',
    )
    iterations: bpy.props.IntProperty(name="Iterations", default=1, min=1, soft_max=100)
    auto_iteration_count: bpy.props.BoolProperty(
        name="Auto Iteration Count", default=True,
        description="Scale the iterations with the vertex count (like vanilla)",
    )

    @classmethod
    def poll(cls, context):
        return _session(context) is not None

    def execute(self, context):
        import numpy as np

        session = _session(context)
        lib = engine.capi().lib
        verts_num = convert.mesh_vert_num(session.mesh_ptr)

        mask = np.zeros(verts_num, dtype=np.float32)
        lib.Mesh_readVertFloatAttr(session.mesh_ptr, convert._SC_MASK, mask)
        before = mask.copy()

        iterations = self.iterations
        if self.auto_iteration_count:
            # Vanilla scaling: one pass per 50k vertices.
            iterations = max(1, verts_num // 50000 + 1)

        if self.filter_type in {'CONTRAST_INCREASE', 'CONTRAST_DECREASE'}:
            # Linear gain around 0.5 per pass (vanilla's contrast filter).
            delta = 0.1 if self.filter_type == 'CONTRAST_INCREASE' else -0.1
            gain = 1.0 / (1.0 - 2.0 * delta)
            for _ in range(iterations):
                mask = gain * (mask - 0.5) + 0.5
                np.clip(mask, 0.0, 1.0, out=mask)
        else:
            src, dst = _vert_adjacency(session.mesh_ptr, verts_num)
            counts = np.maximum(np.bincount(src, minlength=verts_num), 1).astype(np.float32)
            for _ in range(iterations):
                if self.filter_type == 'GROW':
                    out = mask.copy()
                    np.maximum.at(out, src, mask[dst])
                elif self.filter_type == 'SHRINK':
                    out = mask.copy()
                    np.minimum.at(out, src, mask[dst])
                else:
                    acc = np.zeros(verts_num, dtype=np.float32)
                    np.add.at(acc, src, mask[dst])
                    mean = acc / counts
                    if self.filter_type == 'SMOOTH':
                        out = mean
                    else:  # SHARPEN
                        out = mask + (mask - mean) * 0.5
                    np.clip(out, 0.0, 1.0, out=out)
                mask = out

        after = np.ascontiguousarray(mask, dtype=np.float32)
        if not lib.Mesh_writeVertFloatAttr(session.mesh_ptr, convert._SC_MASK, after):
            self.report({'ERROR'}, "Engine rejected the mask write")
            return {'CANCELLED'}

        ob = context.active_object
        undo.push_attr(context, ob, session, "Mask Filter", 'VERT_F32',
                       convert._SC_MASK, before.tobytes(), after.tobytes())
        undo._tag_view3d_redraw(context)
        return {'FINISHED'}


class SCULPTCORE_OT_face_sets_create(bpy.types.Operator):
    """Create a face set from the masked faces"""
    bl_idname = "sculptcore.face_sets_create"
    bl_label = "Face Set from Masked"
    # No 'UNDO': attribute-snapshot step, like mask_flood_fill.
    bl_options = {'REGISTER'}

    mode: bpy.props.EnumProperty(
        name="Mode",
        items=(('MASKED', "Masked", "Faces whose vertices are fully masked"),),
        default='MASKED',
    )
    threshold: bpy.props.FloatProperty(
        name="Threshold", default=0.5, min=0.0, max=1.0,
        description="Minimum mask value for a vertex to count as masked",
    )

    @classmethod
    def poll(cls, context):
        return _session(context) is not None

    def execute(self, context):
        import numpy as np

        session = _session(context)
        lib = engine.capi().lib
        verts_num = convert.mesh_vert_num(session.mesh_ptr)
        faces_num = convert.mesh_face_num(session.mesh_ptr)

        mask = np.zeros(verts_num, dtype=np.float32)
        if not lib.Mesh_readVertFloatAttr(session.mesh_ptr, convert._SC_MASK, mask):
            self.report({'WARNING'}, "No mask to create a face set from")
            return {'CANCELLED'}

        corner_verts, face_offsets = convert.mesh_topo_arrays(session.mesh_ptr)

        # A face joins the new set when every corner vertex is masked past
        # the threshold (vanilla from-masked semantics).
        corner_masked = mask[corner_verts] >= self.threshold
        face_masked = np.logical_and.reduceat(corner_masked, face_offsets[:-1])
        if not face_masked.any():
            self.report({'WARNING'}, "No faces are fully masked")
            return {'CANCELLED'}

        before = np.zeros(faces_num, dtype=np.int32)
        lib.Mesh_readFaceIntAttr(session.mesh_ptr, convert._SC_GROUP, before)
        new_id = int(session.mesh().maxFaceGroup()) + 1
        after = before.copy()
        after[face_masked] = new_id

        if not lib.Mesh_writeFaceIntAttr(session.mesh_ptr, convert._SC_GROUP, after):
            self.report({'ERROR'}, "Engine rejected the face-set write")
            return {'CANCELLED'}

        ob = context.active_object
        undo.push_attr(context, ob, session, "Face Set from Masked", 'FACE_I32',
                       convert._SC_GROUP, before.tobytes(), after.tobytes())
        undo._tag_view3d_redraw(context)
        return {'FINISHED'}


class SCULPTCORE_OT_face_set_edit(bpy.types.Operator):
    """Grow or shrink the face set under the cursor"""
    bl_idname = "sculptcore.face_set_edit"
    bl_label = "Edit Face Set"
    # No 'UNDO': attribute-snapshot step.
    bl_options = {'REGISTER'}

    mode: bpy.props.EnumProperty(
        name="Mode",
        items=(
            ('GROW', "Grow Face Set", "Grow the face set by one vertex ring"),
            ('SHRINK', "Shrink Face Set", "Shrink the face set by one vertex ring"),
        ),
        default='GROW',
    )

    @classmethod
    def poll(cls, context):
        return _session(context) is not None

    def invoke(self, context, event):
        # Pick the face set under the cursor, like vanilla. The pick is the
        # engine face index; after dyntopo leaves freelist gaps it may not
        # map onto the compacted column order, so out-of-range picks (and
        # menu invocations, where the mouse is over the menu) fall back to
        # the highest (most recent) face set in execute.
        from . import stroke as stroke_mod

        session = _session(context)
        self._picked = -1
        if context.region is not None and context.region_data is not None:
            hit = stroke_mod._ray_from_coord(
                context, (event.mouse_region_x, event.mouse_region_y), session)
            if hit is not None:
                self._picked = int(hit[2])
        return self.execute(context)

    def execute(self, context):
        import numpy as np

        session = _session(context)
        lib = engine.capi().lib
        faces_num = convert.mesh_face_num(session.mesh_ptr)

        groups = np.zeros(faces_num, dtype=np.int32)
        if not lib.Mesh_readFaceIntAttr(session.mesh_ptr, convert._SC_GROUP, groups):
            self.report({'WARNING'}, "Mesh has no face sets")
            return {'CANCELLED'}

        picked = getattr(self, "_picked", -1)
        target = int(groups[picked]) if 0 <= picked < faces_num else int(groups.max())
        if target <= 0:
            self.report({'WARNING'}, "No face set to edit")
            return {'CANCELLED'}

        corner_verts, face_offsets = convert.mesh_topo_arrays(session.mesh_ptr)
        verts_num = convert.mesh_vert_num(session.mesh_ptr)
        corner_faces = np.repeat(np.arange(faces_num, dtype=np.int64),
                                 np.diff(face_offsets))
        in_set = groups == target
        after = groups.copy()

        if self.mode == 'GROW':
            # Faces touching any vertex of the set join it.
            vert_in_set = np.zeros(verts_num, dtype=bool)
            vert_in_set[corner_verts[in_set[corner_faces]]] = True
            touching = np.zeros(faces_num, dtype=bool)
            touching[corner_faces[vert_in_set[corner_verts]]] = True
            after[touching] = target
        else:
            # Boundary faces of the set take an adjacent outside set's id.
            vert_other = np.full(verts_num, -1, dtype=np.int32)
            outside = ~in_set[corner_faces]
            vert_other[corner_verts[outside]] = groups[corner_faces[outside]]
            corner_other = vert_other[corner_verts]
            corner_other[outside] = -1  # only verts shared with the set matter
            boundary_ids = np.full(faces_num, -1, dtype=np.int32)
            np.maximum.at(boundary_ids, corner_faces, corner_other)
            replace = in_set & (boundary_ids >= 0)
            after[replace] = boundary_ids[replace]

        if np.array_equal(after, groups):
            return {'CANCELLED'}
        if not lib.Mesh_writeFaceIntAttr(session.mesh_ptr, convert._SC_GROUP, after):
            self.report({'ERROR'}, "Engine rejected the face-set write")
            return {'CANCELLED'}

        ob = context.active_object
        undo.push_attr(context, ob, session, "Edit Face Set", 'FACE_I32',
                       convert._SC_GROUP, groups.tobytes(), after.tobytes())
        undo._tag_view3d_redraw(context)
        return {'FINISHED'}


class SCULPTCORE_OT_dyntopo_detail_size_edit(bpy.types.Operator):
    """Interactively change the dyntopo detail size"""
    bl_idname = "sculptcore.dyntopo_detail_size_edit"
    bl_label = "Edit Dyntopo Detail Size"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return _session(context) is not None

    def invoke(self, context, _event):
        # Radial-edit whichever detail property the active detailing mode
        # reads (vanilla's op does the same dispatch, with a fancier gizmo).
        method = context.tool_settings.sculpt.detail_type_method
        if method in {'CONSTANT', 'MANUAL'}:
            prop = "constant_detail_resolution"
        elif method == 'BRUSH':
            prop = "detail_percent"
        else:
            prop = "detail_size"
        bpy.ops.wm.radial_control(
            'INVOKE_DEFAULT',
            data_path_primary="tool_settings.sculpt.{:s}".format(prop))
        return {'FINISHED'}


class SCULPTCORE_OT_uv_project_from_seams(bpy.types.Operator):
    """Generate a UV map from the marked seam edges (planar projection per seam-bounded chart, packed into the 0-1 tile)"""
    bl_idname = "sculptcore.uv_project_from_seams"
    bl_label = "Project UVs from Seams"
    # No 'UNDO': the op pushes its own attribute-snapshot step (undo.py).
    bl_options = {'REGISTER'}

    margin: bpy.props.FloatProperty(
        name="Margin", default=0.01, min=0.0, max=0.25, subtype='FACTOR',
        description="Padding added around each chart before packing",
    )

    @classmethod
    def poll(cls, context):
        return _session(context) is not None

    def execute(self, context):
        import ctypes

        import numpy as np

        session = _session(context)
        ob = context.active_object
        lib = engine.capi().lib
        corners_num = convert.mesh_corner_num(session.mesh_ptr)
        if not corners_num:
            self.report({'ERROR'}, "Mesh has no faces to unwrap")
            return {'CANCELLED'}

        # Pre-state for undo. A mesh entered without a UV map has no engine
        # `uv` column yet; its pre-state is the zero column (the layer itself
        # is not removed on undo — only its values restore).
        before = np.zeros(corners_num * 2, dtype=np.float32)
        lib.Mesh_readAttr(session.mesh_ptr, 4, b"uv", 2,
                          before.ctypes.data_as(ctypes.c_void_p))

        charts = lib.Mesh_generateUVFromSeams(session.mesh_ptr, b"uv",
                                              int(self.margin * 1000))
        if charts <= 0:
            self.report({'ERROR'}, "UV projection produced no charts")
            return {'CANCELLED'}

        after = np.empty(corners_num * 2, dtype=np.float32)
        lib.Mesh_readAttr(session.mesh_ptr, 4, b"uv", 2,
                          after.ctypes.data_as(ctypes.c_void_p))

        # Keep the bridged engine copy of the active UV map in sync, so a
        # later topology rebuild restores the projected UVs, not the stale
        # pre-project layer. The visible layer always follows the engine `uv`
        # column via the uv_dirty flush.
        uv_layer = ob.data.uv_layers.active
        if uv_layer is not None:
            for desc in session.bridged_attrs:
                if desc["name"] == uv_layer.name and desc["bl_domain"] == 'CORNER':
                    lib.Mesh_writeAttr(session.mesh_ptr, desc["engine_domain"],
                                       desc["name_bytes"], desc["engine_type"],
                                       convert._USE_UV,
                                       after.ctypes.data_as(ctypes.c_void_p))
                    break

        session.uv_dirty = True
        undo.push_attr(context, ob, session, "Project UVs from Seams",
                       'CORNER_F32x2', b"uv", before.tobytes(), after.tobytes())
        convert.flush(ob)
        convert.draw_refresh(ob)
        self.report({'INFO'}, "Projected {:d} UV chart{:s}".format(
            charts, "" if charts == 1 else "s"))
        return {'FINISHED'}


class SCULPTCORE_OT_subdivision_set(bpy.types.Operator):
    """Set the multires sculpt level"""
    bl_idname = "sculptcore.subdivision_set"
    bl_label = "Set Multires Level"
    # 'UNDO' pushes a memfile step for the sculpt_levels edit; undoing it
    # re-drives the engine level through the depsgraph handler (P8 C2).
    bl_options = {'REGISTER', 'UNDO'}

    level: bpy.props.IntProperty(name="Level", default=1, soft_min=-6, soft_max=6)
    relative: bpy.props.BoolProperty(
        name="Relative", default=False,
        description="Apply the level as an offset from the current level",
    )

    @classmethod
    def poll(cls, context):
        session = _session(context, allow_multires=True)
        return session is not None and session.multires_ptr

    def execute(self, context):
        ob = context.active_object
        md = multires.modifier(ob)
        if md is None:
            return {'CANCELLED'}
        level = md.sculpt_levels + self.level if self.relative else self.level
        md.sculpt_levels = max(0, min(md.total_levels, level))
        return {'FINISHED'}


_classes = (
    SCULPTCORE_OT_mask_flood_fill,
    SCULPTCORE_OT_mask_filter,
    SCULPTCORE_OT_face_sets_create,
    SCULPTCORE_OT_face_set_edit,
    SCULPTCORE_OT_dyntopo_detail_size_edit,
    SCULPTCORE_OT_uv_project_from_seams,
    SCULPTCORE_OT_subdivision_set,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
