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

from . import engine
from .session import Session

# Blender mask attribute (float, point domain) <-> the engine's mask column.
_BL_MASK = ".sculpt_mask"
_SC_MASK = b".spatial.v.mask"

# Blender face sets (int, face domain) <-> the engine's `group` face attr.
_BL_FACE_SET = ".sculpt_face_set"
_SC_GROUP = b"group"

# Vertex colors <-> the engine's `color` float4 vertex attr. v1 handles the
# active color attribute when it is POINT-domain FLOAT_COLOR (the exact match);
# corner/byte colors are left untouched (a warning is logged on flush).
_SC_COLOR = b"color"
_DEFAULT_COLOR_NAME = "Color"


class ConvertError(RuntimeError):
    pass


def _gather_arrays(mesh):
    """The Mesh ID's topology in Blender's native flat layout."""
    import numpy as np

    verts_num = len(mesh.vertices)
    corners_num = len(mesh.loops)
    faces_num = len(mesh.polygons)

    positions = np.empty(verts_num * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)

    corner_verts = np.empty(corners_num, dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corner_verts)

    face_offsets = np.empty(faces_num + 1, dtype=np.int32)
    if faces_num:
        mesh.polygons.foreach_get("loop_start", face_offsets[:faces_num])
    face_offsets[faces_num] = corners_num

    return positions, corner_verts, face_offsets


def validate(ob):
    """v1 entry rules (sculpt-modifier-coupling research): refuse shape
    keys; warn-and-proceed on enabled modifiers and loose edges."""
    mesh = ob.data
    if mesh.shape_keys is not None:
        raise ConvertError(
            "SculptCore: cannot enter on {!r} — shape keys are not supported".format(ob.name))

    warnings = []
    if any(md.show_viewport for md in ob.modifiers):
        warnings.append("enabled modifiers are ignored while sculpting")
    if any(edge.is_loose for edge in mesh.edges):
        warnings.append("loose edges will not survive topology-changing sculpting")
    for message in warnings:
        print("SculptCore: warning: {:s} ({:s})".format(message, ob.name))


def enter(ob):
    """Build the engine mesh + spatial tree from the Mesh ID and register
    the session."""
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

    session = Session(ob.name, mesh_ptr, tree_ptr, verts_num)
    engine.sessions[ob.name] = session
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


def _flush_color(mesh, mesh_ptr, verts_num):
    """Write the engine `color` attr back into the active POINT/FLOAT_COLOR
    color attribute, creating one when none exists. Leaves corner/byte color
    attributes untouched (logs a warning)."""
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
        attr = mesh.color_attributes.new(_DEFAULT_COLOR_NAME, 'FLOAT_COLOR', 'POINT')
        mesh.color_attributes.active_color = attr
    attr.data.foreach_set("color", values)


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
        mesh.vertices.foreach_set("co", data[:, 1:4].reshape(-1).copy())


def _flush_topology_rebuild(session, mesh):
    """Slow path — topology changed (dyntopo/remesh), so rebuild the Blender
    mesh geometry from a full engine export. Customdata is dropped by
    clear_geometry (matches vanilla dyntopo); the engine-owned v1 layers
    (mask/face-set/color) are re-flushed afterwards onto the new topology.
    Updates the session's sizes/stamp so the next flush is fast again."""
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


def _engine_vert_num(session):
    """Live vertex count of the engine mesh (may differ from the session's
    cached size after a topology change, e.g. an undo that reverted dyntopo)."""
    import ctypes

    nv, nc, nf, cap = (ctypes.c_int(0) for _ in range(4))
    engine.capi().lib.Mesh_arraySizes(session.mesh_ptr, ctypes.byref(nv), ctypes.byref(nc),
                                      ctypes.byref(nf), ctypes.byref(cap))
    return nv.value


def flush(ob):
    """Write engine state back into the Mesh ID. Fast path (positions only)
    while the topology is unchanged; slow path (full geometry rebuild) after
    dyntopo/remesh. Either way the v1 attribute layers are re-flushed."""
    session = engine.sessions.get(ob.name)
    if session is None or not session.mesh_ptr:
        return

    mesh = ob.data
    # The topo stamp catches forward topology edits, but a meshlog undo reverts
    # the topology without rolling the stamp back; a live-vs-Blender vertex-count
    # mismatch catches that case so undo/redo also take the rebuild path.
    if session.topology_changed() or _engine_vert_num(session) != len(mesh.vertices):
        _flush_topology_rebuild(session, mesh)
    else:
        _flush_positions_fast(session, mesh)

    _flush_mask(mesh, session.mesh_ptr, session.verts_num)
    _flush_face_sets(mesh, session.mesh_ptr)
    _flush_color(mesh, session.mesh_ptr, session.verts_num)
    mesh.update()


def exit_(ob):
    """Flush and free the session (re-entrant: forced exits may repeat)."""
    session = engine.sessions.get(ob.name)
    if session is None:
        return
    try:
        flush(ob)
    finally:
        engine.sessions.pop(ob.name, None)
        session.free()


def refresh(ob):
    """Foreign undo replaced the Mesh data: rebuild the engine mesh from the
    (new) Mesh ID; stale engine handles are detectable via the generation."""
    session = engine.sessions.get(ob.name)
    if session is None:
        return
    generation = session.generation + 1
    session.free()
    new_session = enter(ob)
    new_session.generation = generation


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
    if _engine_vert_num(session) != len(ob.data.vertices):
        refresh(ob)
        return True
    return False
