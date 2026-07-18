# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Deferred Mesh write-back verification (stroke-lag work).

The stroke operator no longer flushes the Mesh at dab time or stroke end when
the draw provider is active — the Mesh ID syncs on demand through the mode's
flush callback. This drives the same engine-side stroke the operator would,
skips the explicit flush (as the operator now does), and asserts:

  * the Blender Mesh stays untouched after the stroke (write-back deferred);
  * saving the file syncs it (the C flush trampoline before memfile/save);
  * an unflushed dyntopo stroke (engine ahead of the Mesh in vertex count)
    does NOT trip convert.resync_if_diverged into a session rebuild;
  * mode exit flushes the remaining engine state into the Mesh.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --factory-startup --python claudeMemory/tests/deferred_flush_test.py
Exits nonzero on failure.
"""

import os
import sys
import tempfile

import numpy as np
import bpy

OBJ = "DeferTest"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def positions():
    ob = bpy.data.objects[OBJ]
    co = np.empty(len(ob.data.vertices) * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def engine_positions(session):
    import sculptcore
    from sculptcore_addon import engine

    mgr = engine.manager()
    mesh_obj = session.mesh()
    with sculptcore.construct_from_items(mgr, mgr.get("float"), []) as dump:
        mesh_obj.dumpVertCo(dump)
        data = dump.numpy().reshape(-1, 4)
        return data[:, 1:4].copy()


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import convert
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    context = bpy.context

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.0)
    ob = context.active_object
    ob.name = OBJ
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    session = engine.sessions[OBJ]

    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius, brush.spacing = 0.8, 0.5, 0.1
    brush.writeProps()

    pre = positions()

    # -- stroke with the operator's new finish: no Mesh flush ---------------
    center, normal, _ = strokemod.raycast(session, (0, 0, 5), (0, 0, -1))
    strokemod.stroke_begin(session)
    for _ in range(10):
        strokemod.apply_dab(session, draw, center, normal, 0.5)
    strokemod.stroke_end(session)
    convert.draw_refresh(ob)
    undo_mod.push(context, ob, session)

    if float(np.abs(positions() - pre).max()) > 1e-7:
        _fail("Mesh changed at stroke end (write-back was not deferred)")
    print("PASS: Mesh untouched after the stroke (deferred write-back)")

    engine_now = engine_positions(session)
    if float(np.abs(engine_now - pre).max()) < 1e-3:
        _fail("engine positions did not move (stroke had no effect)")
    print("PASS: engine moved ahead of the Mesh")

    # -- saving syncs the Mesh through the mode flush callback --------------
    tmp = os.path.join(tempfile.gettempdir(), "sculptcore_defer.blend")
    bpy.ops.wm.save_as_mainfile(filepath=tmp)
    if float(np.abs(positions() - engine_positions(session)).max()) > 1e-6:
        _fail("save did not flush the engine state into the Mesh")
    print("PASS: save flushed the deferred state")
    try:
        os.remove(tmp)
    except OSError:
        pass

    # -- unflushed dyntopo stroke must not trip the divergence guard --------
    prog = strokemod.build_program(session, draw)
    params = strokemod.build_dyntopo_params(session, 0.05, 0.02)
    strokemod.stroke_begin(session, has_dyntopo=True)
    for i in range(8):
        c = (center[0] + (i % 4) * 0.02, center[1] + (i // 4) * 0.02, center[2])
        strokemod.apply_dyntopo_dab(session, prog, c, normal, 0.5, params, 500 + i)
    strokemod.stroke_end(session)
    undo_mod.push(context, ob, session)

    blender_count = len(ob.data.vertices)
    engine_count = len(engine_positions(session))
    if engine_count == blender_count:
        _fail("dyntopo stroke did not change the engine vertex count")
    if convert.resync_if_diverged(ob):
        _fail("resync_if_diverged rebuilt the session for an unflushed stroke")
    if engine.sessions[OBJ] is not session:
        _fail("session was replaced despite no foreign change")
    print("PASS: engine-ahead dyntopo stroke does not trip the divergence guard "
          "(engine {:d} vs Mesh {:d} verts)".format(engine_count, blender_count))

    # -- exit flushes the remaining state -----------------------------------
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    if len(bpy.data.objects[OBJ].data.vertices) != engine_count:
        _fail("exit did not flush the dyntopo topology into the Mesh")
    print("PASS: exit flushed the deferred dyntopo topology")

    print("ALL PASS: deferred write-back contract verified.")


if __name__ == "__main__":
    main()
