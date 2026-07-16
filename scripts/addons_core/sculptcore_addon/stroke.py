# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The interactive stroke: a modal operator plus the reusable dab core the
operator and headless tests both drive.

v0 (S2): DRAW brush via ``CommandExecutor.execBrush`` per dab, positions
flushed to the Mesh on a throttle so the viewport updates. The operator
finishes with ``bl_options={'UNDO'}`` so a memfile push (and thus the mode's
flush) brackets each stroke — Tier-1 undo for free. The dab core is
engine-only (no ``bpy`` state), so ``enter -> N synthetic dabs -> exit`` is
scriptable end-to-end.
"""

import bpy

from . import convert, engine, mapping


def _float3(mgr, x, y, z):
    v = mgr.construct("litestl::math::float3")
    v.vec[0] = x
    v.vec[1] = y
    v.vec[2] = z
    return v


def _ensure_brush(session):
    """The session's reusable engine Brush (built once)."""
    if session.brush_obj is None:
        mgr = engine.manager()
        session.brush_obj = mgr.construct("sculptcore::brush::Brush")
    return session.brush_obj


def _ensure_executor(session):
    """The session's CommandExecutor, bound to its tree + brush."""
    if session.executor is None:
        mgr = engine.manager()
        ctor = mgr.get_struct("sculptcore::brush::CommandExecutor").find_constructor("main")
        session.executor = mgr.construct_with(ctor, session.tree(), _ensure_brush(session))
    return session.executor


def stroke_begin(session, *, has_dyntopo=False):
    _ensure_executor(session).beginStep(has_dyntopo)


def apply_dab(session, brush_type, center, normal, radius):
    """Run one dab at an object-space center/normal. `center`/`normal` are
    3-tuples; `brush_type` is the SculptBrushes enum value. Returns the
    number of spatial nodes the dab touched (0 = brush missed the surface)."""
    import sculptcore

    mgr = engine.manager()
    executor = _ensure_executor(session)
    tree = session.tree()

    center_v = _float3(mgr, *center)
    normal_v = _float3(mgr, *normal)
    nodes = mgr.construct("litestl::util::Vector<sculptcore::spatial::SpatialNode*,4>")
    try:
        if not tree.filterNodes(center_v, radius, nodes):
            return 0
        executor.execBrush(session.mesh(), brush_type, nodes, center_v, normal_v)
        return len(sculptcore.BoundVector(mgr, nodes.ptr, nodes.bind_type))
    finally:
        for obj in (nodes, center_v, normal_v):
            obj.dispose()


def stroke_end(session):
    _ensure_executor(session).endStep()
    session.mesh().recalc_normals()


def raycast(session, origin, direction):
    """Cast a ray (object space) against the engine tree; returns a
    (position, normal, face_index) tuple on hit, else None."""
    mgr = engine.manager()
    tree = session.tree()
    orig_v = _float3(mgr, *origin)
    dir_v = _float3(mgr, *direction)
    hit = mgr.construct("sculptcore::spatial::CastRayIsect")
    try:
        if not tree.castRay(orig_v, dir_v, hit):
            return None
        p = tuple(hit.p.vec)
        n = tuple(hit.normal.vec)
        return (p, n, hit.faceIndex)
    finally:
        for obj in (orig_v, dir_v, hit):
            obj.dispose()


class SCULPTCORE_OT_brush_stroke(bpy.types.Operator):
    bl_idname = "sculptcore.brush_stroke"
    bl_label = "SculptCore Stroke"
    # A memfile push on finish brackets the stroke and triggers the mode's
    # flush (Tier-1 undo). Swapped for the wrapped undo type later.
    bl_options = {'UNDO'}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return (
            ob is not None
            and ob.mode == 'CUSTOM'
            and ob.custom_mode == "sculptcore.sculpt"
            and ob.name in engine.sessions
        )

    def invoke(self, context, event):
        ob = context.active_object
        self.session = engine.sessions[ob.name]
        self.brush = context.tool_settings.sculpt.brush
        mgr = engine.manager()
        self.kernel = mapping.kernel_enum(mgr, self.brush) if self.brush else None
        if self.kernel is None:
            self.report({'WARNING'}, "SculptCore: brush type has no kernel")
            return {'CANCELLED'}

        self._last_flush = 0.0
        self._dab_count = 0
        stroke_begin(self.session)
        context.window_manager.modal_handler_add(self)
        # First dab at the invoke location.
        self._dab_at(context, event)
        return {'RUNNING_MODAL'}

    def _dab_at(self, context, event):
        hit = _ray_from_event(context, event, self.session)
        if hit is None:
            return
        position, normal, _face = hit
        world_radius = _world_radius(context, self.brush, position)
        invert = event.ctrl
        unified = context.tool_settings.sculpt.unified_paint_settings
        mapping.apply_brush(
            self.brush, unified, self.session.brush_obj,
            world_radius=world_radius, invert=invert)
        apply_dab(self.session, self.kernel, position, normal, world_radius)
        self._dab_count += 1

        # Phase-0 draw: throttled positions flush + redraw tag.
        import time
        now = time.monotonic()
        if now - self._last_flush > 1.0 / 30.0:
            convert.flush(context.active_object)
            self._last_flush = now
        context.area.tag_redraw()

    def modal(self, context, event):
        if event.type == 'MOUSEMOVE':
            self._dab_at(context, event)
            return {'RUNNING_MODAL'}
        if event.type == 'LEFTMOUSE' and event.value == 'RELEASE':
            stroke_end(self.session)
            convert.flush(context.active_object)
            context.area.tag_redraw()
            return {'FINISHED'}
        if event.type in {'RIGHTMOUSE', 'ESC'}:
            stroke_end(self.session)
            convert.flush(context.active_object)
            return {'CANCELLED'}
        return {'RUNNING_MODAL'}


def _ray_from_event(context, event, session):
    """Unproject the mouse event to an object-space ray and cast it against
    the engine tree."""
    from bpy_extras import view3d_utils

    region = context.region
    rv3d = context.region_data
    ob = context.active_object
    coord = (event.mouse_region_x, event.mouse_region_y)

    origin_world = view3d_utils.region_2d_to_origin_3d(region, rv3d, coord)
    direction_world = view3d_utils.region_2d_to_vector_3d(region, rv3d, coord)

    matrix_inv = ob.matrix_world.inverted()
    origin = matrix_inv @ origin_world
    direction = (matrix_inv.to_3x3() @ direction_world).normalized()
    return raycast(session, tuple(origin), tuple(direction))


def _world_radius(context, brush, position):
    """Object-space dab radius from the brush's pixel size at the dab
    location (vanilla paint_calc_object_space_radius semantics)."""
    from bpy_extras import view3d_utils

    region = context.region
    rv3d = context.region_data
    ob = context.active_object
    unified = context.tool_settings.sculpt.unified_paint_settings
    pixel_size = unified.size if unified.use_unified_size else brush.size

    center_world = ob.matrix_world @ __import__("mathutils").Vector(position)
    offset_2d = view3d_utils.location_3d_to_region_2d(region, rv3d, center_world)
    if offset_2d is None:
        return brush.unprojected_size or 1.0
    offset_2d = offset_2d.copy()
    offset_2d.x += pixel_size
    ray_origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, offset_2d)
    ray_dir = view3d_utils.region_2d_to_vector_3d(region, rv3d, offset_2d)
    # Object-space distance from the dab center to the unprojected edge point
    # at the dab's depth.
    import mathutils
    edge_world = ray_origin + ray_dir * (center_world - ray_origin).length
    matrix_inv = ob.matrix_world.inverted()
    radius = (matrix_inv @ edge_world - matrix_inv @ center_world).length
    return radius or (brush.unprojected_size or 1.0)


def register():
    bpy.utils.register_class(SCULPTCORE_OT_brush_stroke)


def unregister():
    bpy.utils.unregister_class(SCULPTCORE_OT_brush_stroke)
