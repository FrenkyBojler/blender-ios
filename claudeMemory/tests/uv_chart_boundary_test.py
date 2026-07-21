# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Regression: dyntopo corrupted UVs across chart boundaries when no seam/sharp
edges were marked. The engine derives `.boundary.edge.uvchart` flags lazily
(dirty-marked edges only), but seeding UVs never marked anything dirty and the
addon only recomputed the boundary system when seam/sharp flags were seeded —
so a mesh with UVs and no marked edges got zero chart-boundary protection and
dyntopo split/collapsed straight across charts. Mesh_writeCornerFloat2Attr now
marks the mesh boundary-dirty and the addon recomputes at enter whenever UVs
were seeded.

The check: a flat grid with identity UVs, except faces above y=0 get uv.y += 2
(two charts meeting at the middle row; NO seams, NO sharp edges). A dyntopo
stroke along the boundary must (a) find derived uvchart flags in the engine at
enter, and (b) leave every loop's UV equal to its vertex xy plus its face's
chart offset — cross-chart smearing shows up as errors near 2.0.

Run:
    blender --background --factory-startup --python claudeMemory/tests/uv_chart_boundary_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _chart_uv_error(ob):
    """(max, mean) of |uv - (vertex.xy + face chart offset)| over all loops.
    A face's chart is classified by its center's y sign."""
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
    import ctypes

    import sculptcore_addon.engine as engine
    from sculptcore_addon import convert
    from sculptcore_addon import stroke as strokemod

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    bpy.ops.mesh.primitive_grid_add(x_subdivisions=24, y_subdivisions=24, size=2.0)
    ob = bpy.context.active_object
    ob.name = "UVChart"
    mesh = ob.data

    # Identity UVs, then shift every loop of the upper-half faces by (0, 2):
    # two charts whose boundary is exactly the y=0 edge row. No seams, no sharp.
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

    mx0, mean0 = _chart_uv_error(ob)
    if mx0 > 1e-5:
        _fail("chart setup broken (max {:.5f})".format(mx0))

    v0 = len(mesh.vertices)
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions["UVChart"]

    # The fix under test: with UVs seeded and nothing else marked, the engine
    # must have derived uvchart flags at enter.
    lib = engine.capi().lib
    engine_edges = lib.Mesh_edgeCount(session.mesh_ptr)
    buf = np.empty(engine_edges * 2, dtype=np.int32)
    count = lib.Mesh_readEdgeFlags(session.mesh_ptr, b".boundary.edge.uvchart",
                                   buf, engine_edges)
    print("uvchart edges derived at enter:", count)
    if count < 24:
        _fail("no derived uvchart flags after enter ({})".format(count))

    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius, brush.spacing = 0.4, 0.5, 0.1
    brush.writeProps()
    prog = strokemod.build_program(session, draw)
    params = strokemod.build_dyntopo_params(session, 0.05, 0.02)
    strokemod.stroke_begin(session, has_dyntopo=True)
    for i in range(10):
        brush.strength, brush.radius = 0.4, 0.5
        brush.writeProps()
        strokemod.apply_dyntopo_dab(session, prog, (-0.3 + i * 0.07, 0.0, 0.0),
                                    (0, 0, 1), 0.5, params, 700 + i)
    strokemod.stroke_end(session)
    convert.flush(ob)

    mesh = bpy.data.objects["UVChart"].data
    if len(mesh.vertices) <= v0:
        _fail("dyntopo stroke did not remesh")
    mx, mean = _chart_uv_error(bpy.data.objects["UVChart"])
    print("chart-uv error after dyntopo flush: max {:.5f} mean {:.6f} "
          "({} -> {} verts)".format(mx, mean, v0, len(mesh.vertices)))
    # Cross-chart smearing produces errors near the 2.0 chart offset; affine
    # split/collapse interpolation within a chart stays tiny.
    if mx > 0.1 or mean > 0.005:
        _fail("dyntopo smeared UVs across the chart boundary "
              "(max {:.4f}, mean {:.5f})".format(mx, mean))
    bpy.ops.object.custom_mode_toggle()
    mx2, mean2 = _chart_uv_error(bpy.data.objects["UVChart"])
    if mx2 > 0.1 or mean2 > 0.005:
        _fail("exit flush broke chart UVs (max {:.4f})".format(mx2))
    print("ALL PASS: unmarked UV chart boundary preserved through dyntopo.")


if __name__ == "__main__":
    main()


