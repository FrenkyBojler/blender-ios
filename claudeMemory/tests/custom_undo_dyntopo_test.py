# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Dyntopo delta-undo verification (undo-integration P6 §6: "dyntopo stroke ->
undo restores topology + attributes via meshlog topo replay").

A dyntopo stroke changes the vertex/face count; this drives one such stroke,
pushes its CUSTOM_MODE step, then asserts undo restores the original topology
and positions and redo restores the remeshed topology — all through the same
seek-based decode the DRAW path uses, exercising LogChunkTopo replay.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/custom_undo_dyntopo_test.py
Exits nonzero on failure.
"""

import sys

import numpy as np
import bpy

OBJ = "DTundo"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def topo():
    ob = bpy.data.objects[OBJ]
    return len(ob.data.vertices), len(ob.data.polygons)


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
    bpy.context.active_object.name = OBJ
    context = bpy.context

    v0 = topo()
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    session = engine.sessions[OBJ]
    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])

    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius, brush.spacing = 0.5, 0.5, 0.1
    brush.writeProps()

    center, normal, _ = strokemod.raycast(session, (0, 0, 5), (0, 0, -1))
    prog = strokemod.build_program(session, draw)
    params = strokemod.build_dyntopo_params(session, 0.05, 0.02)

    # One dyntopo stroke (remeshes under the brush).
    strokemod.stroke_begin(session, has_dyntopo=True)
    for i in range(12):
        c = (center[0] + (i % 4) * 0.02, center[1] + (i // 4) * 0.02, center[2])
        strokemod.apply_dyntopo_dab(session, prog, c, normal, 0.5, params, 1000 + i)
    strokemod.stroke_end(session)
    ob = bpy.data.objects[OBJ]
    convert.flush(ob)
    undo_mod.push(context, ob, session)

    v1 = topo()
    if not (v1[0] > v0[0]):
        _fail("dyntopo stroke did not add vertices ({} -> {})".format(v0, v1))
    post = np.empty(v1[0] * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", post)
    print("PASS: dyntopo stroke remeshed {} -> {}".format(v0, v1))

    # Undo: topology + positions must return to the original mesh.
    bpy.ops.ed.undo()
    v_undo = topo()
    if v_undo != v0:
        _fail("undo did not restore original topology ({} != {})".format(v_undo, v0))
    print("PASS: dyntopo undo restored original topology {}".format(v_undo))

    # Redo: back to the remeshed topology and exact positions.
    bpy.ops.ed.redo()
    v_redo = topo()
    if v_redo != v1:
        _fail("redo did not restore remeshed topology ({} != {})".format(v_redo, v1))
    ob = bpy.data.objects[OBJ]
    post2 = np.empty(v_redo[0] * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", post2)
    d = float(np.abs(post2 - post).max())
    if d > 1e-6:
        _fail("redo positions differ from post-stroke by {:.3e}".format(d))
    print("PASS: dyntopo redo restored remeshed topology + positions exactly")

    print("ALL PASS: dyntopo delta undo verified.")


if __name__ == "__main__":
    main()
