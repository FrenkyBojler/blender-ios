# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Repro: repeated dyntopo stroke + undo cycles corrupt UVs, including near (but
outside) the stroke region.

Identity UV map on a flat grid (uv == vertex.xy), so after every undo the UVs
must return to identity exactly; any residue is corruption. Each cycle runs a
dyntopo stroke, flushes, pushes a CUSTOM_MODE step, undoes it, and measures
|uv - xy| over all loops (engine-side UVs are checked too, to separate meshlog
restore errors from flush-order errors).

Run:
    blender --background --factory-startup --python claudeMemory/tests/uv_undo_cycle_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

OBJ = "UVUndo"
CYCLES = 6


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _uv_vs_xy(ob):
    """(max, mean, nbad) of |uv - loop.vertex.xy| over all loops."""
    mesh = ob.data
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    cv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    mesh.uv_layers.active.data.foreach_get("uv", uv)
    d = np.sqrt(((uv.reshape(-1, 2) - pos[cv][:, :2]) ** 2).sum(axis=1))
    return float(d.max()), float(d.mean()), int((d > 0.01).sum())


def _engine_uv_vs_xy(session, ob):
    """Same metric on the ENGINE's uv attr (read in export corner order)."""
    import ctypes
    import sculptcore_addon.engine as engine

    mesh = ob.data
    lib = engine.capi().lib
    values = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    if not lib.Mesh_readAttr(session.mesh_ptr, 4, b"uv", 2,
                             values.ctypes.data_as(ctypes.c_void_p)):
        return -1.0, -1.0, -1
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    cv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    d = np.sqrt(((values.reshape(-1, 2) - pos[cv][:, :2]) ** 2).sum(axis=1))
    return float(d.max()), float(d.mean()), int((d > 0.01).sum())


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import convert
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    bpy.ops.mesh.primitive_grid_add(x_subdivisions=24, y_subdivisions=24, size=2.0)
    ob = bpy.context.active_object
    ob.name = OBJ
    mesh = ob.data
    cv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    layer = mesh.uv_layers.active or mesh.uv_layers.new(name="UVMap")
    layer.data.foreach_set("uv", pos.reshape(-1, 3)[cv][:, :2].reshape(-1))
    context = bpy.context

    v0 = len(mesh.vertices)
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    prev_session = None

    ok = True
    for cycle in range(CYCLES):
        # Re-fetch per cycle: an undo landing on a memfile step may rebuild the
        # session; stale executor/program handles would then drive a dead mesh.
        session = engine.sessions[OBJ]
        if prev_session is not None and session is not prev_session:
            print("cycle {}: session was rebuilt".format(cycle))
        prev_session = session
        import os
        brush = strokemod._ensure_brush(session)
        # Mirror the interactive stroke: autosmooth chain + UV reprojection on
        # the smooth family AND inside dyntopo's tangential smoothing. Each
        # ingredient is env-toggleable to bisect undo corruption.
        brush.reproject_uvs = os.environ.get("UV_UNDO_BRUSH_REPROJ", "1") == "1"
        smooth_factor = float(os.environ.get("UV_UNDO_AUTOSMOOTH", "0.5"))
        prog = strokemod.build_program(session, draw, smooth_factor=smooth_factor)
        params = strokemod.build_dyntopo_params(session, 0.05, 0.02)
        params.do_smooth = os.environ.get("UV_UNDO_DT_SMOOTH", "1") == "1"
        params.smooth_lambda = 0.5
        params.reproject_uvs = os.environ.get("UV_UNDO_DT_REPROJ", "1") == "1"
        import os
        dabs = int(os.environ.get("UV_UNDO_DABS", "10"))
        use_dyntopo = os.environ.get("UV_UNDO_DYNTOPO", "1") == "1"
        if not use_dyntopo:
            params = None
        strokemod.stroke_begin(session, has_dyntopo=use_dyntopo)
        applied = []
        for i in range(dabs):
            brush.strength, brush.radius, brush.spacing = 0.4, 0.5, 0.1
            brush.writeProps()
            executor = strokemod._ensure_executor(session)
            t = i / max(dabs - 1, 1)
            center_v = strokemod._float3(mgr, -0.85 + t * 1.7,
                                         0.4 * np.sin(t * 6.0), 0.0)
            normal_v = strokemod._float3(mgr, 0.0, 0.0, 1.0)
            try:
                executor.setGrabAccumAdd(False)
                applied.append(int(executor.applyDab(
                    prog, center_v, normal_v, 0.5, params,
                    700 + cycle * 100 + i)))
            finally:
                center_v.dispose()
                normal_v.dispose()
        print("cycle {}: dab splits+collapses {}".format(cycle, applied))
        strokemod.stroke_end(session)
        v_engine = convert.mesh_vert_num(session.mesh_ptr)
        ob = bpy.data.objects[OBJ]
        convert.flush(ob)
        undo_mod.push(context, ob, session)
        v_stroke = len(bpy.data.objects[OBJ].data.vertices)
        # Probe: did the DRAW displacement land at all (max |z| after flush)?
        zpos = np.empty(v_stroke * 3, dtype=np.float32)
        ob.data.vertices.foreach_get("co", zpos)
        print("cycle {}: engine verts after stroke {}, max|z| {:.4f}".format(
            cycle, v_engine, float(np.abs(zpos.reshape(-1, 3)[:, 2]).max())))
        if use_dyntopo and v_engine <= v0:
            _fail("cycle {}: dyntopo did not remesh (engine verts {})".format(
                cycle, v_engine))

        bpy.ops.ed.undo()
        ob = bpy.data.objects[OBJ]
        v_undo = len(ob.data.vertices)
        mx, mean, nbad = _uv_vs_xy(ob)
        emx, emean, enbad = _engine_uv_vs_xy(session, ob)
        print("cycle {}: verts {} -> stroke {} -> undo {} | "
              "blender uv err max {:.5f} mean {:.6f} bad {} | "
              "engine uv err max {:.5f} mean {:.6f} bad {}".format(
                  cycle, v0, v_stroke, v_undo, mx, mean, nbad, emx, emean, enbad))
        if v_undo != v0:
            _fail("cycle {}: undo did not restore topology ({} != {})".format(
                cycle, v_undo, v0))
        if mx > 0.01 or (emx > 0.01 and enbad >= 0):
            ok = False

    if not ok:
        _fail("UVs corrupted across stroke+undo cycles (see per-cycle log)")
    print("ALL PASS: UVs identity-exact after every stroke+undo cycle.")


if __name__ == "__main__":
    main()
