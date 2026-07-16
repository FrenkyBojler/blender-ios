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

    session = Session(ob.name, mesh_ptr, tree_ptr, verts_num)
    engine.sessions[ob.name] = session
    return session


def flush(ob):
    """Write engine state back into the Mesh ID (fast path: positions only,
    valid while no topology op ran)."""
    import numpy as np

    session = engine.sessions.get(ob.name)
    if session is None or not session.mesh_ptr:
        return

    if session.topology_changed():
        # Slow path (full re-export + Mesh rebuild) lands with the attribute
        # copy stage; topology ops are not reachable until dyntopo is wired.
        raise ConvertError(
            "SculptCore: topology changed but the rebuild path is not implemented yet")

    mgr = engine.manager()
    import sculptcore

    mesh_obj = mgr.get_bound_pointer(
        mgr.get("sculptcore::mesh::Mesh"), session.mesh_ptr, deref=False)
    with sculptcore.construct_from_items(mgr, mgr.get("float"), []) as dump:
        mesh_obj.dumpVertCo(dump)
        data = dump.numpy().reshape(-1, 4).copy()

    indices = data[:, 0].astype(np.int64)
    positions = np.empty((session.verts_num, 3), dtype=np.float32)
    positions[indices] = data[:, 1:4]

    mesh = ob.data
    mesh.vertices.foreach_set("co", positions.ravel())
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
