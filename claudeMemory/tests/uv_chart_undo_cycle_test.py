# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Regression: repeated dyntopo stroke + undo corrupted UVs at chart boundaries.
The meshlog topo replay restores elements with raw alloc/release, which left
the replayed region without boundary-dirty marks (so the derived uvchart flags
were never re-computed — the next stroke smeared UVs across charts) and with a
stale `n_ngon_faces` (so on an n-gon mesh the next stroke's dyntopo silently
refused every op). MeshLog::undo/redo now re-marks the replayed region
boundary-dirty and recounts n-gons.

Setup mirrors uv_chart_boundary_test (two charts meeting at y=0, NO seams or
sharp edges, quad grid); each cycle strokes along the boundary, checks the
chart offsets survived the stroke, undoes, and checks the exact restore.

Run:
    blender --background --factory-startup --python claudeMemory/tests/uv_chart_undo_cycle_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

OBJ = "UVChartUndo"
CYCLES = 4


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _chart_uv_error(ob):
    """(max, mean) of |uv - (vertex.xy + face chart offset)| over all loops."""
    mesh = ob.data
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    cv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    mesh.uv_layers.active.data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    centers = np.empty(len(mesh.polygons) * 3, dtype=np.float32)
    mesh.polygons.foreach_get("center", centers)
    totals = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get("loop_total", totals)
    off_y = np.repeat(np.where(centers.reshape(-1, 3)[:, 1] > 0.0, 2.0, 0.0), totals)
    expect = pos[cv][:, :2].copy()
    expect[:, 1] += off_y
    d = np.sqrt(((uv - expect) ** 2).sum(axis=1))
    return float(d.max()), float(d.mean())


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
    uv = pos.reshape(-1, 3)[cv][:, :2].copy()
    centers = np.empty(len(mesh.polygons) * 3, dtype=np.float32)
    mesh.polygons.foreach_get("center", centers)
    totals = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get("loop_total", totals)
    uv[:, 1] += np.repeat(np.where(centers.reshape(-1, 3)[:, 1] > 0.0, 2.0, 0.0), totals)
    layer = mesh.uv_layers.active or mesh.uv_layers.new(name="UVMap")
    layer.data.foreach_set("uv", uv.reshape(-1))
    context = bpy.context

    v0 = len(mesh.vertices)
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])

    for cycle in range(CYCLES):
        session = engine.sessions[OBJ]
        brush = strokemod._ensure_brush(session)
        # Interactive-stroke config: autosmooth chain + UV reprojection (the
        # corner element-store capture path) + dyntopo tangential smoothing.
        brush.reproject_uvs = True
        prog = strokemod.build_program(session, draw, smooth_factor=0.5)
        params = strokemod.build_dyntopo_params(session, 0.05, 0.02)
        params.do_smooth = True
        params.smooth_lambda = 0.5
        params.reproject_uvs = True
        strokemod.stroke_begin(session, has_dyntopo=True)
        for i in range(10):
            brush.strength, brush.radius, brush.spacing = 0.4, 0.5, 0.1
            brush.writeProps()
            strokemod.apply_dyntopo_dab(session, prog,
                                        (-0.3 + i * 0.07, 0.0, 0.0),
                                        (0, 0, 1), 0.5, params,
                                        900 + cycle * 100 + i)
        strokemod.stroke_end(session)
        v_engine = convert.mesh_vert_num(session.mesh_ptr)
        ob = bpy.data.objects[OBJ]
        convert.flush(ob)
        undo_mod.push(context, ob, session)
        if v_engine <= v0:
            _fail("cycle {}: dyntopo did not remesh after undo "
                  "(engine verts {})".format(cycle, v_engine))
        smx, smean = _chart_uv_error(bpy.data.objects[OBJ])
        bpy.ops.ed.undo()
        umx, umean = _chart_uv_error(bpy.data.objects[OBJ])
        print("cycle {}: stroke verts {} | chart err post-stroke max {:.5f} "
              "mean {:.6f} | post-undo max {:.5f} mean {:.6f}".format(
                  cycle, v_engine, smx, smean, umx, umean))
        if smx > 0.1 or smean > 0.005:
            _fail("cycle {}: stroke smeared UVs across the chart boundary "
                  "(max {:.4f})".format(cycle, smx))
        if umx > 1e-4:
            _fail("cycle {}: undo did not restore chart UVs exactly "
                  "(max {:.5f})".format(cycle, umx))
        if len(bpy.data.objects[OBJ].data.vertices) != v0:
            _fail("cycle {}: undo did not restore topology".format(cycle))

    print("ALL PASS: chart boundaries preserved and restored across "
          "{} stroke+undo cycles.".format(CYCLES))


if __name__ == "__main__":
    main()
