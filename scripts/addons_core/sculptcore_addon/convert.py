# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Mesh <-> SculptCore conversion (positions-only slice).

The invariant everything else relies on: the Blender Mesh ID is the
persistent store — ``flush()`` makes it match the engine's state on demand
(called by Blender before memfile undo encode, file save and render, via the
mode's ``flush`` callback). Engine-side state that has no Mesh
representation (the spatial tree) is rebuilt on ``refresh``/re-enter, never
serialized.

v1 layer policy (attributes beyond positions) lands with the mask/face-set/
color/UV copy stage; until then non-position data is untouched in the Mesh
and stays valid because topology ops are not yet reachable.
"""

from . import engine, multires
from .session import Session

# Blender mask attribute (float, point domain) <-> the engine's mask column.
_BL_MASK = ".sculpt_mask"
_SC_MASK = b".spatial.v.mask"

# Blender face sets (int, face domain) <-> the engine's `group` face attr.
_BL_FACE_SET = ".sculpt_face_set"
_SC_GROUP = b"group"

# Blender edge flags <-> the engine's boundary bool edge attrs (P11). The
# engine derives its own edges, so both directions key edges by vertex pair,
# never by index (see _load_edge_flags/_flush_edge_flags).
_EDGE_FLAG_MAP = (
    ("uv_seam", b".boundary.edge.seam"),
    ("sharp_edge", b".boundary.edge.sharp"),
)

# Vertex colors <-> the engine's `color` float4 vertex attr. v1 handles the
# active color attribute when it is POINT-domain FLOAT_COLOR (the exact match);
# corner/byte colors are left untouched (a warning is logged on flush).
_SC_COLOR = b"color"
_DEFAULT_COLOR_NAME = "Color"


class ConvertError(RuntimeError):
    pass


def _read_positions(mesh, out):
    """Bulk-read vertex positions into `out` (a `verts_num * 3` float32 array).
    The `position` attribute is a contiguous float3 array, so `foreach_get` on
    it is an order of magnitude faster than the `vertices.co` collection
    accessor at scale (~40 ms -> ~3 ms at 1M verts)."""
    attr = mesh.attributes.get("position")
    if attr is not None and attr.data_type == 'FLOAT_VECTOR':
        attr.data.foreach_get("vector", out)
    else:
        mesh.vertices.foreach_get("co", out)


def _gather_arrays(mesh):
    """The Mesh ID's topology in Blender's native flat layout."""
    import numpy as np

    verts_num = len(mesh.vertices)
    corners_num = len(mesh.loops)
    faces_num = len(mesh.polygons)

    positions = np.empty(verts_num * 3, dtype=np.float32)
    _read_positions(mesh, positions)

    # The `.corner_vert` builtin attribute is a contiguous int array; reading it
    # is ~2x faster than `loops.vertex_index`. Fall back for meshes that predate
    # it.
    corner_verts = np.empty(corners_num, dtype=np.int32)
    cv_attr = mesh.attributes.get(".corner_vert")
    if cv_attr is not None and cv_attr.data_type == 'INT':
        cv_attr.data.foreach_get("value", corner_verts)
    else:
        mesh.loops.foreach_get("vertex_index", corner_verts)

    face_offsets = np.empty(faces_num + 1, dtype=np.int32)
    if faces_num:
        mesh.polygons.foreach_get("loop_start", face_offsets[:faces_num])
    face_offsets[faces_num] = corners_num

    return positions, corner_verts, face_offsets


def validate(ob, ignore_multires=False):
    """v1 entry rules (sculpt-modifier-coupling research): refuse shape
    keys; warn-and-proceed on enabled modifiers and loose edges. Multires
    sessions pass ``ignore_multires`` — their modifier is supported (converted,
    not ignored), so it is excluded from the enabled-modifier warning."""
    mesh = ob.data
    if mesh.shape_keys is not None:
        raise ConvertError(
            "SculptCore: cannot enter on {!r} — shape keys are not supported".format(ob.name))

    warnings = []
    if any(md.show_viewport for md in ob.modifiers
           if not (ignore_multires and md.type == 'MULTIRES')):
        warnings.append("enabled modifiers are ignored while sculpting")
    if any(edge.is_loose for edge in mesh.edges):
        warnings.append("loose edges will not survive topology-changing sculpting")
    for message in warnings:
        print("SculptCore: warning: {:s} ({:s})".format(message, ob.name))


def enter(ob):
    """Build the engine mesh + spatial tree from the Mesh ID and register
    the session. Objects with a multires modifier take the P8 stack path;
    everything else converts the plain Mesh."""
    md = multires.modifier(ob)
    if md is not None and md.total_levels >= 1:
        return _enter_multires(ob, md)
    validate(ob)
    capi = engine.capi()

    positions, corner_verts, face_offsets = _gather_arrays(ob.data)
    verts_num = len(positions) // 3

    mesh_ptr = capi.lib.Mesh_fromArrays(
        positions, verts_num,
        corner_verts, len(corner_verts),
        face_offsets, len(face_offsets) - 1,
    )
    if not mesh_ptr:
        raise ConvertError("SculptCore: engine rejected mesh {!r}".format(ob.data.name))

    tree_ptr = capi.lib.Mesh_buildSpatialTree(mesh_ptr, 0, 0, 0)
    if not tree_ptr:
        capi.lib.freeMesh(mesh_ptr)
        raise ConvertError("SculptCore: spatial tree build failed for {!r}".format(ob.data.name))

    _load_mask(ob.data, mesh_ptr, verts_num)
    _load_face_sets(ob.data, mesh_ptr)
    _load_color(ob.data, mesh_ptr, verts_num)
    has_uv = _load_uv(ob.data, mesh_ptr)
    _load_edge_flags(ob.data, mesh_ptr, recompute=has_uv)

    session = Session(ob.name, mesh_ptr, tree_ptr, verts_num)
    engine.sessions[ob.name] = session

    # Seed the remaining user attribute layers so they ride the engine through
    # dyntopo + undo and can be rebuilt after a topology change.
    _load_bridged_attrs(ob.data, mesh_ptr, session)

    # Register the tree for external-provider viewport draw, keyed by the
    # object's session_uid (the key Blender's draw path passes). Switch it to the
    # dynamic per-attribute layout (color@0, uv@1) so the provider exposes them,
    # then fill the GPU-node buffers once so the initial geometry draws.
    session.draw_key = int(ob.session_uid)
    lib = engine.capi().lib
    lib.sc_external_draw_register(session.draw_key, tree_ptr)
    lib.sc_external_draw_enable_dynamic(tree_ptr)
    lib.sc_external_draw_update(session.draw_key)

    return session


def _enter_multires(ob, md):
    """Multires enter (P8): build an engine Multires stack over the base cage,
    import the object's displaced top-level surface (CD_MDISPS via the
    evaluated modifier), and register the stack's top-level tree for draw. The
    modifier's viewport display is suppressed while the mode is active — the
    provider draws the engine surface — and restored on exit. The v1 attribute
    layers (mask/face-set/color/UV) stay untouched (grid channels are A4)."""
    import ctypes

    import bpy
    import numpy as np

    validate(ob, ignore_multires=True)
    lib = engine.capi().lib
    context = bpy.context
    level = md.total_levels

    base_arrays = _gather_arrays(ob.data)

    # The displaced top-level surface in subdiv-vertex order: evaluate the
    # modifier at its top level, then suppress it for the mode's lifetime.
    prev_show = md.show_viewport
    prev_levels = md.levels
    md.show_viewport = True
    md.levels = level
    depsgraph = context.evaluated_depsgraph_get()
    depsgraph.update()
    eval_mesh = ob.evaluated_get(depsgraph).data
    top = np.empty(len(eval_mesh.vertices) * 3, dtype=np.float64)
    eval_mesh.vertices.foreach_get("co", top)
    top = top.reshape(-1, 3)
    md.levels = prev_levels
    md.show_viewport = False

    mr = cage = None
    try:
        mr, cage = multires.build_engine(base_arrays, level)
        mr_map = multires.build_map(context, base_arrays, mr, level)
        if len(top) != len(mr_map.blender_to_engine_sample):
            raise ConvertError(
                "SculptCore: multires subdiv vertex count mismatch on {!r}".format(ob.name))
        multires.import_displacement(mr, mr_map, top)
    except Exception:
        if mr:
            lib.Multires_free(mr)
        if cage:
            lib.freeMesh(cage)
        md.show_viewport = prev_show
        raise

    # The import rematerialized the seeded level; fetch the current views.
    lib.Multires_setActiveLevel(mr, level)
    mesh_ptr = lib.Multires_activeMesh(mr)
    tree_ptr = lib.Multires_activeTree(mr)

    # Mask (A4): seed the top-level engine mask from the grid paint mask.
    # Exchange happens at the top level only — see _flush_multires.
    depsgraph = context.evaluated_depsgraph_get()
    multires.import_mask(ob, depsgraph, mesh_ptr, mr_map)

    session = Session(ob.name, mesh_ptr, tree_ptr, _mesh_vert_num(mesh_ptr))
    session.blender_verts_num = len(ob.data.vertices)
    session.multires_ptr = mr
    session.cage_ptr = cage
    session.multires_map = mr_map
    session.multires_level = level
    session.multires_active_level = level
    session.multires_show_viewport = prev_show
    engine.sessions[ob.name] = session

    session.draw_key = int(ob.session_uid)
    lib.sc_external_draw_register(session.draw_key, tree_ptr)
    lib.sc_external_draw_update(session.draw_key)

    # Honor the modifier's sculpt level (C2); the import left the top active.
    sculpt_level = min(max(md.sculpt_levels, 1), level)
    if sculpt_level != level:
        set_multires_level(ob, sculpt_level)

    # The imported state is the first undo push's pre-state (C4).
    session.multires_last_blob = multires_store_blob(session)
    return session


def _load_face_sets(mesh, mesh_ptr):
    """Seed the engine `group` face attr from the Blender `.sculpt_face_set`
    attribute (int, face). No-op when the mesh carries no face sets."""
    import numpy as np

    attr = mesh.attributes.get(_BL_FACE_SET)
    if attr is None or attr.domain != 'FACE' or attr.data_type != 'INT':
        return
    values = np.empty(len(mesh.polygons), dtype=np.int32)
    attr.data.foreach_get("value", values)
    engine.capi().lib.Mesh_writeFaceIntAttr(mesh_ptr, _SC_GROUP, values)


def _flush_face_sets(mesh, mesh_ptr):
    """Write the engine `group` face attr back into `.sculpt_face_set`,
    creating it on first use. No-op when the engine has no face groups."""
    import numpy as np

    values = np.empty(len(mesh.polygons), dtype=np.int32)
    if not engine.capi().lib.Mesh_readFaceIntAttr(mesh_ptr, _SC_GROUP, values):
        return
    attr = mesh.attributes.get(_BL_FACE_SET)
    if attr is None:
        attr = mesh.attributes.new(_BL_FACE_SET, 'INT', 'FACE')
    attr.data.foreach_set("value", values)


def _point_float_color(mesh):
    """The active color attribute if it is the POINT/FLOAT_COLOR match the
    engine's `color` float4 vertex attr expects, else None."""
    attr = mesh.color_attributes.active_color
    if attr is not None and attr.domain == 'POINT' and attr.data_type == 'FLOAT_COLOR':
        return attr
    return None


def _load_color(mesh, mesh_ptr, verts_num):
    """Seed the engine `color` attr from the active POINT/FLOAT_COLOR color
    attribute. No-op when there is none of that kind."""
    import numpy as np

    attr = _point_float_color(mesh)
    if attr is None:
        return
    values = np.empty(verts_num * 4, dtype=np.float32)
    attr.data.foreach_get("color", values)
    engine.capi().lib.Mesh_writeVertFloat4Attr(mesh_ptr, _SC_COLOR, values)


def _flush_color(mesh, mesh_ptr, verts_num, color_name=None):
    """Write the engine `color` attr back into the active POINT/FLOAT_COLOR
    color attribute, creating one when none exists (under `color_name`, the name
    recorded at enter, so a rebuild keeps the layer's identity). Leaves
    corner/byte color attributes untouched (logs a warning)."""
    import numpy as np

    values = np.empty(verts_num * 4, dtype=np.float32)
    if not engine.capi().lib.Mesh_readVertFloat4Attr(mesh_ptr, _SC_COLOR, values):
        return
    attr = _point_float_color(mesh)
    if attr is None:
        if mesh.color_attributes.active_color is not None:
            print("SculptCore: active color attribute is not POINT/FLOAT_COLOR; "
                  "painted colors not written back")
            return
        attr = mesh.color_attributes.new(color_name or _DEFAULT_COLOR_NAME,
                                         'FLOAT_COLOR', 'POINT')
        mesh.color_attributes.active_color = attr
    attr.data.foreach_set("color", values)


def _load_edge_flags(mesh, mesh_ptr, recompute=False):
    """Seed the engine boundary edge flags (seam/sharp) from the Blender edge
    bool attributes. Engine vertex indices equal Blender indices at enter
    (Mesh_fromArrays creates verts in order), so edges are keyed by their
    vertex pair. Recomputes the boundary classification when anything was
    seeded — or when the caller passes ``recompute`` (UVs were seeded, which
    marks the whole mesh boundary-dirty) — so BSMOOTH/dyntopo see the seam/
    sharp features *and* the derived UV-chart boundaries from the first
    stroke."""
    import numpy as np

    lib = engine.capi().lib
    edges_num = len(mesh.edges)
    if not edges_num:
        return
    edge_verts = None
    seeded = recompute
    for bl_name, sc_name in _EDGE_FLAG_MAP:
        attr = mesh.attributes.get(bl_name)
        if attr is None or attr.domain != 'EDGE' or attr.data_type != 'BOOLEAN':
            continue
        values = np.empty(edges_num, dtype=np.uint8)
        attr.data.foreach_get("value", values.view(np.bool_))
        if not values.any():
            continue
        if edge_verts is None:
            edge_verts = np.empty(edges_num * 2, dtype=np.int32)
            mesh.edges.foreach_get("vertices", edge_verts)
        lib.Mesh_writeEdgeFlagsByVerts(mesh_ptr, sc_name, edge_verts, values, edges_num)
        seeded = True
    if seeded:
        lib.Mesh_recomputeBoundary(mesh_ptr)


def _flush_edge_flags(session, mesh, vert_map):
    """Recreate the Blender seam/sharp edge attributes from the engine
    boundary flags after a topology rebuild (`calc_edges=True` regenerated the
    edges with all flags dropped). `vert_map` maps engine vertex index ->
    rebuilt Blender index (Mesh_toArrays). Engine edges are matched to Blender
    edges by sorted vertex pair; flags whose edge no longer exists are
    silently dropped (dyntopo may have collapsed it)."""
    import numpy as np

    lib = engine.capi().lib
    edges_num = len(mesh.edges)
    engine_edges = lib.Mesh_edgeCount(session.mesh_ptr)
    if not edges_num or not engine_edges:
        return

    def pair_keys(pairs):
        lo = np.minimum(pairs[:, 0], pairs[:, 1]).astype(np.int64)
        hi = np.maximum(pairs[:, 0], pairs[:, 1]).astype(np.int64)
        return (lo << 32) | hi

    bl_order = bl_keys = None
    buf = np.empty(engine_edges * 2, dtype=np.int32)
    for bl_name, sc_name in _EDGE_FLAG_MAP:
        count = lib.Mesh_readEdgeFlags(session.mesh_ptr, sc_name, buf, engine_edges)
        if count <= 0:
            continue
        if bl_keys is None:
            bl_edge_verts = np.empty(edges_num * 2, dtype=np.int32)
            mesh.edges.foreach_get("vertices", bl_edge_verts)
            keys = pair_keys(bl_edge_verts.reshape(-1, 2))
            bl_order = np.argsort(keys)
            bl_keys = keys[bl_order]
        pairs = vert_map[buf[:count * 2].reshape(-1, 2)]
        keys = pair_keys(pairs[np.all(pairs >= 0, axis=1)])
        idx = np.searchsorted(bl_keys, keys)
        idx[idx >= edges_num] = edges_num - 1
        matched = bl_order[idx[bl_keys[idx] == keys]]
        if not len(matched):
            continue
        values = np.zeros(edges_num, dtype=np.bool_)
        values[matched] = True
        attr = mesh.attributes.get(bl_name)
        if attr is None:
            attr = mesh.attributes.new(bl_name, 'BOOLEAN', 'EDGE')
        attr.data.foreach_set("value", values)


def _load_uv(mesh, mesh_ptr):
    """Seed the engine `uv` corner attribute from the active UV map (per-loop
    float2, loop order = the engine's corner order). Returns True when UVs
    were seeded (the engine marks the mesh boundary-dirty so the derived
    UV-chart edge flags can be recomputed). No-op with no UV map."""
    import numpy as np

    uv_layer = mesh.uv_layers.active
    if uv_layer is None:
        return False
    values = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    # A UV map is a CORNER-domain FLOAT2 attribute; reading it through the
    # attribute API is a contiguous memcpy, ~300x faster than the per-element
    # `uv_layers.active.data.uv` accessor (~870 ms -> ~3 ms at 4M corners).
    uv_attr = mesh.attributes.get(uv_layer.name)
    if uv_attr is not None and uv_attr.domain == 'CORNER' and uv_attr.data_type == 'FLOAT2':
        uv_attr.data.foreach_get("vector", values)
    else:
        uv_layer.data.foreach_get("uv", values)
    engine.capi().lib.Mesh_writeCornerFloat2Attr(mesh_ptr, b"uv", values)
    return True


def _flush_uv(mesh, mesh_ptr):
    """Write the engine `uv` corner attr back into the active UV map, creating
    one when the mesh has none. Only called when the engine UVs diverged from
    the Mesh (session.uv_dirty — the UV-project operator / UV reprojection);
    regular strokes never touch UVs, so the default flush skips this."""
    import ctypes

    import numpy as np

    values = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    # Engine domain CORNER (4) / AttrType FLOAT2 (2); see _DOMAIN_TO_ENGINE.
    if not engine.capi().lib.Mesh_readAttr(mesh_ptr, 4, b"uv", 2,
                                           values.ctypes.data_as(ctypes.c_void_p)):
        return
    uv_layer = mesh.uv_layers.active
    if uv_layer is None:
        uv_layer = mesh.uv_layers.new(name="UVMap")
        if uv_layer is None:
            return
    uv_attr = mesh.attributes.get(uv_layer.name)
    if uv_attr is not None and uv_attr.domain == 'CORNER' and uv_attr.data_type == 'FLOAT2':
        uv_attr.data.foreach_set("vector", values)
    else:
        uv_layer.data.foreach_set("uv", values)


# Generic user-attribute bridge
#
# User attribute layers (extra UV maps, color layers, custom vertex/face
# attributes, material indices, ...) are seeded into the engine on enter, so
# dyntopo interpolates them onto new geometry and the meshlog reverts them on
# undo. The topology-rebuild path drops all Blender customdata (clear_geometry),
# so these layers are recreated from the engine afterwards. Positions, topology
# builtins, and the dedicated brush-target layers (mask/face-set/active color)
# have their own paths and are skipped here.

# Blender attribute domain -> engine ElemType flag. The engine edge domain has
# no stable correspondence to Blender's derived edges, so edge attributes
# (creases/seams/sharp) are not bridged yet.
_DOMAIN_TO_ENGINE = {'POINT': 1, 'CORNER': 4, 'FACE': 16}

# Engine AttrType values (extern/sculptcore/source/mesh/attribute_enums.h).
_AT_FLOAT, _AT_FLOAT2, _AT_FLOAT3, _AT_FLOAT4 = 1, 2, 4, 8
_AT_BOOL, _AT_INT, _AT_INT2 = 16, 32, 64
# Engine AttrUse values (semantic tag; UV/COLOR keep a re-imported layer typed).
_USE_NONE, _USE_COLOR, _USE_UV = 0, 2, 4

# Blender data_type -> (engine AttrType, component count, numpy dtype, the
# `foreach_get`/`foreach_set` property on the layer's data). The engine type may
# be wider than the Blender one (a byte color rides the engine's FLOAT4); the
# Blender type is recreated exactly from the stored descriptor on read-back.
_ATTR_TYPE_MAP = {
    'FLOAT':        (_AT_FLOAT,  1, "float32", "value"),
    'FLOAT2':       (_AT_FLOAT2, 2, "float32", "vector"),
    'FLOAT_VECTOR': (_AT_FLOAT3, 3, "float32", "vector"),
    'FLOAT_COLOR':  (_AT_FLOAT4, 4, "float32", "color"),
    'BYTE_COLOR':   (_AT_FLOAT4, 4, "float32", "color"),
    'INT':          (_AT_INT,    1, "int32",   "value"),
    'INT32_2D':     (_AT_INT2,   2, "int32",   "value"),
    'BOOLEAN':      (_AT_BOOL,   1, "uint8",   "value"),
    'QUATERNION':   (_AT_FLOAT4, 4, "float32", "value"),
}

# Never bridged: "position" (its own path), and every "."-prefixed layer —
# Blender's convention for internal/managed data (topology links `.corner_vert`,
# selections `.select_vert`, and the dedicated `.sculpt_mask`/`.sculpt_face_set`).
# User-created attributes never start with a dot. (The active color name is added
# dynamically in _load_bridged_attrs.)
_SKIP_ATTR_NAMES = {"position"}


def _bridge_use(data_type, domain, engine_type):
    """The engine AttrUse tag for a bridged layer, so a re-imported UV map or
    color layer keeps its semantic type."""
    if data_type in {'FLOAT_COLOR', 'BYTE_COLOR'}:
        return _USE_COLOR
    if engine_type == _AT_FLOAT2 and domain == 'CORNER':
        return _USE_UV
    return _USE_NONE


def _load_bridged_attrs(mesh, mesh_ptr, session):
    """Seed every user attribute layer into the engine and record a descriptor
    so :func:`_flush_bridged_attrs` can recreate it after a topology rebuild.
    Skips positions/topology builtins, the dedicated mask/face-set/color layers,
    the engine edge domain, and unsupported Blender types (logged)."""
    import ctypes

    import numpy as np

    lib = engine.capi().lib
    skip = set(_SKIP_ATTR_NAMES)
    color = _point_float_color(mesh)
    if color is not None:
        skip.add(color.name)
        session.color_attr_name = color.name

    session.bridged_attrs = []
    for attr in mesh.attributes:
        if attr.name in skip or attr.name.startswith("."):
            continue
        engine_domain = _DOMAIN_TO_ENGINE.get(attr.domain)
        if engine_domain is None:
            continue
        mapping = _ATTR_TYPE_MAP.get(attr.data_type)
        if mapping is None:
            print("SculptCore: attribute {!r} ({:s}/{:s}) is unsupported and "
                  "will be dropped on topology change".format(
                      attr.name, attr.domain, attr.data_type))
            continue
        engine_type, ncomp, dtype, prop = mapping
        values = np.empty(len(attr.data) * ncomp, dtype=dtype)
        attr.data.foreach_get(prop, values)
        name_bytes = attr.name.encode("utf-8")
        use = _bridge_use(attr.data_type, attr.domain, engine_type)
        lib.Mesh_writeAttr(mesh_ptr, engine_domain, name_bytes, engine_type, use,
                           values.ctypes.data_as(ctypes.c_void_p))
        session.bridged_attrs.append({
            "name": attr.name,
            "name_bytes": name_bytes,
            "bl_domain": attr.domain,
            "bl_type": attr.data_type,
            "engine_domain": engine_domain,
            "engine_type": engine_type,
            "ncomp": ncomp,
            "dtype": dtype,
            "prop": prop,
        })


def _flush_bridged_attrs(session, mesh):
    """Recreate every bridged user attribute layer on the rebuilt Blender mesh
    from the engine's (interpolated / undo-reverted) values. Called on the
    topology-rebuild path only — the fast path leaves Blender customdata intact.
    A layer the engine no longer carries is skipped (leaves no stale data)."""
    import ctypes

    import numpy as np

    lib = engine.capi().lib
    domain_len = {'POINT': len(mesh.vertices), 'CORNER': len(mesh.loops),
                  'FACE': len(mesh.polygons)}
    for desc in session.bridged_attrs:
        count = domain_len[desc["bl_domain"]]
        values = np.empty(count * desc["ncomp"], dtype=desc["dtype"])
        if not lib.Mesh_readAttr(session.mesh_ptr, desc["engine_domain"],
                                 desc["name_bytes"], desc["engine_type"],
                                 values.ctypes.data_as(ctypes.c_void_p)):
            continue
        try:
            attr = mesh.attributes.get(desc["name"])
            if attr is None:
                attr = mesh.attributes.new(desc["name"], desc["bl_type"], desc["bl_domain"])
            attr.data.foreach_set(desc["prop"], values)
        except (RuntimeError, TypeError) as error:
            # A reserved/builtin name Blender refuses to recreate, or a
            # domain-size mismatch; skip rather than abort the whole flush.
            print("SculptCore: could not restore attribute {!r}: {:s}".format(
                desc["name"], str(error)))


def _load_mask(mesh, mesh_ptr, verts_num):
    """Seed the engine mask column from the Blender `.sculpt_mask` attribute
    (float, point). No-op when the mesh carries no mask."""
    import numpy as np

    attr = mesh.attributes.get(_BL_MASK)
    if attr is None or attr.domain != 'POINT' or attr.data_type != 'FLOAT':
        return
    values = np.empty(verts_num, dtype=np.float32)
    attr.data.foreach_get("value", values)
    engine.capi().lib.Mesh_writeVertFloatAttr(mesh_ptr, _SC_MASK, values)


def _flush_mask(mesh, mesh_ptr, verts_num):
    """Write the engine mask column back into the Blender `.sculpt_mask`
    attribute, creating it on first use. No-op when the engine has no mask."""
    import numpy as np

    values = np.empty(verts_num, dtype=np.float32)
    if not engine.capi().lib.Mesh_readVertFloatAttr(mesh_ptr, _SC_MASK, values):
        return
    attr = mesh.attributes.get(_BL_MASK)
    if attr is None:
        attr = mesh.attributes.new(_BL_MASK, 'FLOAT', 'POINT')
    attr.data.foreach_set("value", values)


def _flush_positions_fast(session, mesh):
    """Positions-only write-back. `dumpVertCo` emits (engine_index, x, y, z)
    per live vert in the engine's live-iteration order — the same order
    `Mesh_toArrays` uses, so the i-th row is Blender vert i regardless of the
    freelist gaps dyntopo leaves in the engine index space. Write the coords in
    order; the index column is ignored."""
    import sculptcore

    mgr = engine.manager()
    mesh_obj = mgr.get_bound_pointer(
        mgr.get("sculptcore::mesh::Mesh"), session.mesh_ptr, deref=False)
    with sculptcore.construct_from_items(mgr, mgr.get("float"), []) as dump:
        mesh_obj.dumpVertCo(dump)
        data = dump.numpy().reshape(-1, 4)
        coords = data[:, 1:4].reshape(-1).copy()
        # Write through the contiguous `position` attribute (the fast-read path
        # in reverse); ~10x faster than `vertices.foreach_set("co", ...)`.
        attr = mesh.attributes.get("position")
        if attr is not None and attr.data_type == 'FLOAT_VECTOR':
            attr.data.foreach_set("vector", coords)
        else:
            mesh.vertices.foreach_set("co", coords)


def _flush_topology_rebuild(session, mesh):
    """Slow path — topology changed (dyntopo/remesh), so rebuild the Blender
    mesh geometry from a full engine export. Customdata is dropped by
    clear_geometry; the dedicated mask/face-set/color layers are re-flushed by
    the caller, and the bridged user attributes (UV maps, colors, custom attrs)
    are recreated here onto the new topology from their engine copies. Updates
    the session's sizes/stamp so the next flush is fast again."""
    import ctypes

    import numpy as np

    lib = engine.capi().lib
    nv, nc, nf, cap = (ctypes.c_int(0) for _ in range(4))
    lib.Mesh_arraySizes(session.mesh_ptr, ctypes.byref(nv), ctypes.byref(nc),
                        ctypes.byref(nf), ctypes.byref(cap))
    positions = np.empty(nv.value * 3, dtype=np.float32)
    corner_verts = np.empty(nc.value, dtype=np.int32)
    face_offsets = np.empty(nf.value + 1, dtype=np.int32)
    vert_map = np.empty(cap.value, dtype=np.int32)
    lib.Mesh_toArrays(session.mesh_ptr, positions, corner_verts, face_offsets, vert_map)

    # Bulk rebuild (no per-face Python — dyntopo meshes get large). Build the
    # vert/loop/poly domains directly from the flat arrays, then let update()
    # derive the edges.
    mesh.clear_geometry()
    mesh.vertices.add(nv.value)
    mesh.vertices.foreach_set("co", positions)
    mesh.loops.add(nc.value)
    mesh.loops.foreach_set("vertex_index", corner_verts)
    mesh.polygons.add(nf.value)
    mesh.polygons.foreach_set("loop_start", face_offsets[:nf.value])
    mesh.polygons.foreach_set("loop_total", np.diff(face_offsets))
    mesh.update(calc_edges=True)

    session.verts_num = nv.value
    session.topo_stamp = lib.Mesh_topoStamp(session.mesh_ptr)

    # Recreate the user attribute layers clear_geometry dropped, from their
    # engine copies (interpolated by dyntopo / reverted by the meshlog on undo).
    _flush_bridged_attrs(session, mesh)
    _flush_edge_flags(session, mesh, vert_map)


def _mesh_counts(mesh_ptr):
    """Live (verts, corners, faces, capacity) of an engine mesh. Counts may
    differ from the session's cached sizes after a topology change (e.g. an
    undo that reverted dyntopo); capacity sizes Mesh_toArrays' vert_map
    output (the engine index space including freelist gaps)."""
    import ctypes

    nv, nc, nf, cap = (ctypes.c_int(0) for _ in range(4))
    engine.capi().lib.Mesh_arraySizes(mesh_ptr, ctypes.byref(nv), ctypes.byref(nc),
                                      ctypes.byref(nf), ctypes.byref(cap))
    return nv.value, nc.value, nf.value, cap.value


def _mesh_vert_num(mesh_ptr):
    return _mesh_counts(mesh_ptr)[0]


def mesh_vert_num(mesh_ptr):
    """Live vertex count (public: the attribute ops/undo size their columns
    with this)."""
    return _mesh_counts(mesh_ptr)[0]


def mesh_face_num(mesh_ptr):
    """Live face count (public: see mesh_vert_num)."""
    return _mesh_counts(mesh_ptr)[2]


def mesh_corner_num(mesh_ptr):
    """Live corner (loop) count (public: see mesh_vert_num)."""
    return _mesh_counts(mesh_ptr)[1]


def mesh_positions(mesh_ptr):
    """Live vertex positions (float32, flat xyz) in live-iteration order."""
    import numpy as np

    verts_num, corners_num, faces_num, capacity = _mesh_counts(mesh_ptr)
    positions = np.empty(verts_num * 3, dtype=np.float32)
    corner_verts = np.empty(corners_num, dtype=np.int32)
    face_offsets = np.empty(faces_num + 1, dtype=np.int32)
    vert_map = np.empty(max(capacity, 1), dtype=np.int32)
    engine.capi().lib.Mesh_toArrays(mesh_ptr, positions, corner_verts,
                                    face_offsets, vert_map)
    return positions


def mesh_topo_arrays(mesh_ptr):
    """Dump the engine topology in live-iteration order: (corner_verts,
    face_offsets) as int32 arrays, matching the order of the attribute
    columns (see _flush_positions_fast on why the order lines up)."""
    import numpy as np

    verts_num, corners_num, faces_num, capacity = _mesh_counts(mesh_ptr)
    positions = np.empty(verts_num * 3, dtype=np.float32)
    corner_verts = np.empty(corners_num, dtype=np.int32)
    face_offsets = np.empty(faces_num + 1, dtype=np.int32)
    vert_map = np.empty(max(capacity, 1), dtype=np.int32)
    engine.capi().lib.Mesh_toArrays(mesh_ptr, positions, corner_verts,
                                    face_offsets, vert_map)
    return corner_verts, face_offsets


def _rebind_multires_views(session, active_level):
    """Point the session at the stack's current active mesh/tree. When the
    slot pointers changed (level switch, or an eviction rematerialized the
    slot), every cached wrapper bound to the old slot is reset, the meshlog
    history is dropped (the generation bump makes its undo steps decode as
    no-ops, like a refresh), and the draw provider moves to the new tree."""
    lib = engine.capi().lib
    mesh_ptr = lib.Multires_activeMesh(session.multires_ptr)
    tree_ptr = lib.Multires_activeTree(session.multires_ptr)
    session.multires_active_level = active_level
    if mesh_ptr == session.mesh_ptr and tree_ptr == session.tree_ptr:
        return
    session.mesh_ptr = mesh_ptr
    session.tree_ptr = tree_ptr
    session.verts_num = _mesh_vert_num(mesh_ptr)
    session.topo_stamp = lib.Mesh_topoStamp(mesh_ptr)
    session.generation += 1
    # The executor points at the meshlog; dispose it first (as in free()).
    for obj in (session.executor, session.meshlog):
        if obj is not None and not getattr(obj, "_disposed", False):
            obj.dispose()
    session.executor = None
    session.meshlog = None
    session.meshlog_cursor = 0
    session.mesh_obj = None
    if session.draw_key:
        lib.sc_external_draw_unregister(session.draw_key)
        lib.sc_external_draw_register(session.draw_key, tree_ptr)
        lib.sc_external_draw_update(session.draw_key)


def multires_store_blob(session):
    """Snapshot the multires displacement store as bytes (C4 undo payload).
    The active level is written back first so pending slot-mesh edits are
    included. Returns None on failure (or for plain-Mesh sessions)."""
    import ctypes

    if not session.multires_ptr:
        return None
    lib = engine.capi().lib
    lib.Multires_writeback(session.multires_ptr, session.multires_active_level)
    size = ctypes.c_int(0)
    buf = lib.Multires_serializeStore(session.multires_ptr, ctypes.byref(size))
    if not buf or size.value <= 0:
        return None
    try:
        return ctypes.string_at(buf, size.value)
    finally:
        lib.freeMeshBuffer(buf)


def multires_restore_blob(ob, session, blob, level):
    """Restore a store snapshot and re-activate `level` (C4 undo fallback for
    steps whose meshlog died — a level switch or blob restore reset it). The
    restore invalidates every derived slot, so the views always rebind.
    Returns False when the blob no longer fits the cage (foreign rebuild)."""
    lib = engine.capi().lib
    if not lib.Multires_restoreStore(session.multires_ptr, blob, len(blob)):
        print("SculptCore: multires undo blob no longer matches {!r}; "
              "step skipped".format(ob.name))
        return False
    actual = lib.Multires_setActiveLevel(session.multires_ptr, level)
    _rebind_multires_views(session, actual)
    # The store now equals this blob; a new stroke branches from here.
    session.multires_last_blob = blob
    return True


def set_multires_level(ob, level):
    """Switch a multires session's active engine level (C2). The engine
    writes the outgoing level's edits back into the store; finer detail rides
    on top of coarser edits through the displacement cascade. The paint mask
    lives on the level mesh (dropped with the evicted slot), so leaving the
    top level persists it to the grid paint mask and returning re-seeds it."""
    import bpy

    session = engine.sessions.get(ob.name)
    if session is None or not session.multires_ptr:
        return
    lib = engine.capi().lib
    level = min(max(int(level), 1), session.multires_level)
    top = session.multires_level
    was = session.multires_active_level
    if was == top and level != top:
        multires.export_mask(ob, bpy.context.evaluated_depsgraph_get(),
                             session.mesh_ptr, session.multires_map)
    actual = lib.Multires_setActiveLevel(session.multires_ptr, level)
    _rebind_multires_views(session, actual)
    if actual == top and was != top:
        multires.import_mask(ob, bpy.context.evaluated_depsgraph_get(),
                             session.mesh_ptr, session.multires_map)


def _flush_multires(ob, session):
    """Multires write-back: bake the engine stack's top-level surface into the
    object's CD_MDISPS. The bake builds its own subdivision from the base mesh,
    so the suppressed modifier viewport state does not affect it. Dumping the
    top level moves the engine's active level there; restore the sculpt level
    afterwards (a no-op rebind while the slots stay resident). The paint mask
    is exchanged at the top level only (the mask attribute lives on the level
    mesh; a level switch persists it — see set_multires_level)."""
    import bpy

    depsgraph = bpy.context.evaluated_depsgraph_get()
    multires.export_bake(ob, depsgraph, session.multires_ptr, session.multires_map)
    if session.multires_active_level == session.multires_level:
        multires.export_mask(ob, depsgraph, session.mesh_ptr, session.multires_map)
    lib = engine.capi().lib
    if session.multires_active_level != session.multires_level:
        lib.Multires_setActiveLevel(session.multires_ptr, session.multires_active_level)
        _rebind_multires_views(session, session.multires_active_level)
    if session.draw_key:
        lib.sc_external_draw_update(session.draw_key)


def flush(ob):
    """Write engine state back into the Mesh ID. Fast path (positions only)
    while the topology is unchanged; slow path (full geometry rebuild) after
    dyntopo/remesh. Either way the v1 attribute layers are re-flushed.
    Multires sessions instead bake the engine surface into CD_MDISPS."""
    session = engine.sessions.get(ob.name)
    if session is None or not session.mesh_ptr:
        return
    if session.multires_ptr:
        _flush_multires(ob, session)
        return

    mesh = ob.data
    # The topo stamp catches forward topology edits, but a meshlog undo reverts
    # the topology without rolling the stamp back; a live-vs-Blender vertex-count
    # mismatch catches that case so undo/redo also take the rebuild path.
    if session.topology_changed() or _mesh_vert_num(session.mesh_ptr) != len(mesh.vertices):
        _flush_topology_rebuild(session, mesh)
    else:
        _flush_positions_fast(session, mesh)

    _flush_mask(mesh, session.mesh_ptr, session.verts_num)
    _flush_face_sets(mesh, session.mesh_ptr)
    _flush_color(mesh, session.mesh_ptr, session.verts_num, session.color_attr_name)
    if session.uv_dirty:
        _flush_uv(mesh, session.mesh_ptr)
    mesh.update()
    session.blender_verts_num = len(mesh.vertices)

    # Refresh the external-provider GPU-node buffers so the viewport (which
    # draws from the provider, not this Mesh) reflects the stroke.
    if session.draw_key:
        engine.capi().lib.sc_external_draw_update(session.draw_key)


def draw_refresh(ob):
    """Refresh the external-draw GPU buffers and re-sync the object in the
    draw manager (a display-only SHADING tag, vanilla sculpt's per-step tag —
    without it the cached object sync never re-queries the provider). This is
    the per-dab viewport update; the Mesh itself stays untouched."""
    session = engine.sessions.get(ob.name)
    if session is not None and session.draw_key:
        engine.capi().lib.sc_external_draw_update(session.draw_key)
        ob.update_tag(refresh={'SHADING'})


def exit_(ob):
    """Flush and free the session (re-entrant: forced exits may repeat).
    Multires sessions also restore the modifier's viewport display."""
    session = engine.sessions.get(ob.name)
    if session is None:
        return
    try:
        flush(ob)
    finally:
        if session.draw_key:
            engine.capi().lib.sc_external_draw_unregister(session.draw_key)
        if session.multires_ptr:
            md = multires.modifier(ob)
            if md is not None:
                md.show_viewport = session.multires_show_viewport
        engine.sessions.pop(ob.name, None)
        session.free()


def refresh(ob):
    """Foreign undo replaced the Mesh data: rebuild the engine mesh from the
    (new) Mesh ID; stale engine handles are detectable via the generation."""
    session = engine.sessions.get(ob.name)
    if session is None:
        return
    generation = session.generation + 1
    was_multires = session.multires_ptr is not None
    prev_show = session.multires_show_viewport
    session.free()
    new_session = enter(ob)
    new_session.generation = generation
    if was_multires and new_session.multires_ptr:
        # Mid-mode the modifier is already suppressed, so the re-enter recorded
        # False as the restore state; keep the original pre-enter state (an
        # undo that restored the DNA to visible re-records it correctly).
        new_session.multires_show_viewport = prev_show


def resync_if_diverged(ob):
    """Rebuild the session when the Blender Mesh no longer matches the engine —
    a foreign memfile undo changed the topology under a custom-undo mode (whose
    delta undo skips the generic refresh, see ed_undo.cc A3). Cheap: a vertex-
    count mismatch is the topology-change signal. Sculpting on a stale engine
    mesh would otherwise corrupt or crash; the rebuilt session bumps its
    generation so orphaned meshlog steps decode as no-ops (see undo.py).

    Returns True when it rebuilt (the caller's cached session handle is stale)."""
    session = engine.sessions.get(ob.name)
    if session is None or not session.mesh_ptr:
        return False
    if session.multires_ptr:
        # The Blender mesh is the cage; compare against the engine's cage
        # copy (the level meshes are derived and never match ob.data).
        if _mesh_vert_num(session.cage_ptr) != len(ob.data.vertices):
            refresh(ob)
            return True
        return False
    # Compare against the Blender count at the last sync, not the live engine
    # count: with deferred write-back the engine legitimately runs ahead of
    # the Mesh (e.g. an unflushed dyntopo stroke), and only a Mesh that
    # changed under us signals a foreign edit.
    if len(ob.data.vertices) != session.blender_verts_num:
        refresh(ob)
        return True
    return False
