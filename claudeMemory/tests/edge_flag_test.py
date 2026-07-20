# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Seam/sharp edge-flag migration (P11 A1): Blender's `uv_seam`/`sharp_edge` edge
bools must seed the engine's `.boundary.edge.seam`/`.sharp` attributes on
enter (with the vertex classification recomputed), survive a no-stroke exit
untouched, survive the dyntopo topology-rebuild flush (edges regenerated, flags
re-matched by vertex pair), and restore exactly on undo.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/edge_flag_test.py
Exits nonzero on failure.
"""

import ctypes
import sys

import numpy as np
import bpy

OBJ = "EdgeFlags"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _edge_bool(mesh, name):
    attr = mesh.attributes.get(name)
    if attr is None:
        return None
    out = np.empty(len(mesh.edges), dtype=np.bool_)
    attr.data.foreach_get("value", out)
    return out


def _edge_verts(mesh):
    out = np.empty(len(mesh.edges) * 2, dtype=np.int32)
    mesh.edges.foreach_get("vertices", out)
    return out.reshape(-1, 2)


def _flag_midpoints(mesh, name):
    """Sorted set of flagged-edge midpoints (rounded) — an index-free identity
    for the flagged edge set that survives vertex renumbering."""
    flags = _edge_bool(mesh, name)
    if flags is None:
        return set()
    ev = _edge_verts(mesh)[flags]
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    mids = (pos[ev[:, 0]] + pos[ev[:, 1]]) * 0.5
    return {tuple(np.round(m, 4)) for m in mids}


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod
    from sculptcore_addon import convert

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=8, radius=1.0)
    ob = bpy.context.active_object
    ob.name = OBJ
    mesh = ob.data
    context = bpy.context

    # Mark the equator ring as seams and one meridian as sharp: both far from
    # the dab site (the +z pole), so dyntopo must not disturb them.
    ev = _edge_verts(mesh)
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    z = pos[:, 2]
    seam = (np.abs(z[ev[:, 0]]) < 0.05) & (np.abs(z[ev[:, 1]]) < 0.05)
    xy_on_meridian = (np.abs(pos[:, 1]) < 1e-4) & (pos[:, 0] > 1e-4)
    sharp = xy_on_meridian[ev[:, 0]] & xy_on_meridian[ev[:, 1]] & \
        (z[ev[:, 0]] < 0.1) & (z[ev[:, 1]] < 0.1)
    if not seam.any() or not sharp.any():
        _fail("test setup: empty seam ({}) or sharp ({}) set".format(
            int(seam.sum()), int(sharp.sum())))
    mesh.attributes.new("uv_seam", 'BOOLEAN', 'EDGE').data.foreach_set("value", seam)
    mesh.attributes.new("sharp_edge", 'BOOLEAN', 'EDGE').data.foreach_set("value", sharp)

    seam0, sharp0 = seam.copy(), sharp.copy()
    seam_mids0 = _flag_midpoints(mesh, "uv_seam")
    sharp_mids0 = _flag_midpoints(mesh, "sharp_edge")

    # -- Enter: engine must carry the flags and a recomputed classification.
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    session = engine.sessions[OBJ]
    lib = engine.capi().lib

    ec = lib.Mesh_edgeCount(session.mesh_ptr)
    buf = np.empty(max(ec, 1) * 2, dtype=np.int32)
    n_seam = lib.Mesh_readEdgeFlags(session.mesh_ptr, b".boundary.edge.seam", buf, ec)
    if n_seam != int(seam.sum()):
        _fail("engine seam count {} != Blender {}".format(n_seam, int(seam.sum())))
    got_pairs = {tuple(sorted(p)) for p in buf[:n_seam * 2].reshape(-1, 2).tolist()}
    want_pairs = {tuple(sorted(p)) for p in ev[seam].tolist()}
    if got_pairs != want_pairs:
        _fail("engine seam edges differ from the marked Blender edges")
    n_sharp = lib.Mesh_readEdgeFlags(session.mesh_ptr, b".boundary.edge.sharp", buf, ec)
    if n_sharp != int(sharp.sum()):
        _fail("engine sharp count {} != Blender {}".format(n_sharp, int(sharp.sum())))

    vclass = np.zeros(len(mesh.vertices), dtype=np.int32)
    ok = lib.Mesh_readAttr(session.mesh_ptr, 1, b".boundary.vert.class", 32,
                           vclass.ctypes.data_as(ctypes.c_void_p))
    seam_verts = np.unique(ev[seam])
    if not ok or not np.all(vclass[seam_verts] & 4):  # BC_SEAM
        _fail("vertex classification missing BC_SEAM on seam verts (recompute not run?)")
    print("PASS: enter seeded engine seam/sharp flags + classification")

    # -- No-stroke exit: Blender attributes untouched (fast path).
    bpy.ops.object.custom_mode_toggle()
    mesh = bpy.data.objects[OBJ].data
    if not np.array_equal(_edge_bool(mesh, "uv_seam"), seam0) or \
       not np.array_equal(_edge_bool(mesh, "sharp_edge"), sharp0):
        _fail("no-stroke exit changed the edge flag attributes")
    print("PASS: no-stroke exit left edge flags untouched")

    # -- Dyntopo stroke at the +z pole → topology rebuild; flags must survive.
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base2")
    session = engine.sessions[OBJ]
    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius, brush.spacing = 0.5, 0.4, 0.1
    brush.writeProps()
    center, normal, _ = strokemod.raycast(session, (0, 0, 5), (0, 0, -1))
    prog = strokemod.build_program(session, draw)
    params = strokemod.build_dyntopo_params(session, 0.05, 0.02)
    strokemod.stroke_begin(session, has_dyntopo=True)
    for i in range(12):
        c = (center[0] + (i % 4) * 0.02, center[1] + (i // 4) * 0.02, center[2])
        strokemod.apply_dyntopo_dab(session, prog, c, normal, 0.4, params, 1000 + i)
    strokemod.stroke_end(session)
    convert.flush(ob)
    undo_mod.push(context, ob, session)

    mesh = bpy.data.objects[OBJ].data
    if len(mesh.vertices) <= len(seam0):  # sanity: remesh happened
        pass
    seam1 = _edge_bool(mesh, "uv_seam")
    sharp1 = _edge_bool(mesh, "sharp_edge")
    if seam1 is None or sharp1 is None:
        _fail("edge flag attribute dropped on the dyntopo rebuild")
    if _flag_midpoints(mesh, "uv_seam") != seam_mids0:
        _fail("seam edge set changed through the rebuild (midpoint mismatch)")
    if _flag_midpoints(mesh, "sharp_edge") != sharp_mids0:
        _fail("sharp edge set changed through the rebuild (midpoint mismatch)")
    print("PASS: seam/sharp flags survived the dyntopo topology rebuild")

    # -- Undo → the original flag sets. Compared as midpoint sets, not arrays:
    # the undo rebuild regenerates edges via calc_edges, whose order need not
    # match the original primitive's edge order.
    bpy.ops.ed.undo()
    mesh = bpy.data.objects[OBJ].data
    if len(mesh.edges) != len(seam0):
        _fail("undo did not restore the original edge count")
    seam_u = _edge_bool(mesh, "uv_seam")
    sharp_u = _edge_bool(mesh, "sharp_edge")
    if seam_u is None or int(seam_u.sum()) != int(seam0.sum()):
        _fail("undo seam count {} != original {}".format(
            None if seam_u is None else int(seam_u.sum()), int(seam0.sum())))
    if sharp_u is None or int(sharp_u.sum()) != int(sharp0.sum()):
        _fail("undo sharp count {} != original {}".format(
            None if sharp_u is None else int(sharp_u.sum()), int(sharp0.sum())))
    if _flag_midpoints(mesh, "uv_seam") != seam_mids0:
        _fail("seam edge set not restored on undo (midpoint mismatch)")
    if _flag_midpoints(mesh, "sharp_edge") != sharp_mids0:
        _fail("sharp edge set not restored on undo (midpoint mismatch)")
    print("PASS: undo restored the seam/sharp edge sets")

    print("ALL PASS: edge-flag migration (enter/exit/rebuild/undo).")


if __name__ == "__main__":
    main()
