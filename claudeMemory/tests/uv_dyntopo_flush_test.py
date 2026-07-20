# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Regression: the topology-rebuild flush scrambled every UV after a dyntopo
stroke. Mesh_toArrays exports corners in face-walk order, but the corner-domain
attribute marshaling iterated element-index order — identical on a fresh mesh,
divergent once dyntopo fragments corner ids — so the rebuilt Blender mesh got
each corner attribute on the wrong loop (the engine, drawing via its own face
walks, looked fine). The C API now marshals the corner domain in export order.

The check exploits an identity UV map (uv = vertex xy on a flat grid): dyntopo
split/collapse interpolation is affine-exact, so after the stroke + flush every
loop must still satisfy uv == loop.vertex.xy — any order mismatch shows up as
a large error on most loops.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/uv_dyntopo_flush_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _uv_vs_xy(ob):
    """(max, mean) of |uv - loop.vertex.xy| over all loops."""
    mesh = ob.data
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    pos = pos.reshape(-1, 3)
    cv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    mesh.uv_layers.active.data.foreach_get("uv", uv)
    d = np.sqrt(((uv.reshape(-1, 2) - pos[cv][:, :2]) ** 2).sum(axis=1))
    return float(d.max()), float(d.mean())


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import convert
    from sculptcore_addon import stroke as strokemod

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

    bpy.ops.mesh.primitive_grid_add(x_subdivisions=24, y_subdivisions=24, size=2.0)
    ob = bpy.context.active_object
    ob.name = "UVFlush"
    mesh = ob.data
    # Identity UV map: uv = vertex xy.
    cv = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    pos = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", pos)
    layer = mesh.uv_layers.active or mesh.uv_layers.new(name="UVMap")
    layer.data.foreach_set("uv", pos.reshape(-1, 3)[cv][:, :2].reshape(-1))

    v0 = len(mesh.vertices)
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions["UVFlush"]
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

    mesh = bpy.data.objects["UVFlush"].data
    if len(mesh.vertices) <= v0:
        _fail("dyntopo stroke did not remesh")
    # The stroke displaces along +z only, so uv == xy must survive the rebuild.
    # Splits/collapses interpolate affinely; allow a small slack for the
    # collapse wedge blend.
    mx, mean = _uv_vs_xy(bpy.data.objects["UVFlush"])
    print("uv-vs-xy after dyntopo flush: max {:.5f} mean {:.6f} "
          "({} -> {} verts)".format(mx, mean, v0, len(mesh.vertices)))
    if mx > 0.05 or mean > 0.005:
        _fail("corner attributes landed on wrong loops after the rebuild "
              "(max {:.4f}, mean {:.5f})".format(mx, mean))
    # Exit must hold too (second flush over the already-rebuilt mesh).
    bpy.ops.object.custom_mode_toggle()
    mx2, mean2 = _uv_vs_xy(bpy.data.objects["UVFlush"])
    if mx2 > 0.05 or mean2 > 0.005:
        _fail("exit flush scrambled UVs (max {:.4f})".format(mx2))
    print("ALL PASS: corner-domain flush order preserved through dyntopo rebuild.")


if __name__ == "__main__":
    main()
