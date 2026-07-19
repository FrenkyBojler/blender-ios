# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Robustness checks for Tier-2 delta undo (undo-integration P6 §6):

  1. A Python exception raised inside undo_decode must be reported, not crash
     the undo stack (the RNA trampoline catches it).
  2. Undoing after the mode has exited (session + meshlog freed) must not crash;
     the step decodes as an engine no-op and the mesh is whatever the exit
     flushed / memfile restored.

These are the "degrade safely" guarantees; exactness across the exit boundary is
tracked separately (plan §4). Pure-Python (no C rebuild).

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/custom_undo_robustness_test.py
"""

import sys

import numpy as np
import bpy

OBJ = "Robust"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def _stroke(session, strokemod, undo_mod, context, draw):
    ob = bpy.data.objects[OBJ]
    strokemod.stroke_begin(session, has_dyntopo=False)
    for _ in range(4):
        strokemod.apply_dab(session, draw, (0.0, 0.0, 1.0), (0.0, 0.0, 1.0), 0.6)
    strokemod.stroke_end(session)
    from sculptcore_addon import convert
    convert.flush(ob)
    undo_mod.push(context, ob, session)


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1.0)
    bpy.context.active_object.name = OBJ
    context = bpy.context

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    session = engine.sessions[OBJ]
    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius = 0.5, 0.5
    brush.writeProps()

    # -- 1. exception inside undo_decode must not crash --------------------
    _stroke(session, strokemod, undo_mod, context, draw)
    _stroke(session, strokemod, undo_mod, context, draw)

    orig_decode = undo_mod.decode

    def boom(*args, **kwargs):
        raise RuntimeError("intentional decode failure")

    undo_mod.decode = boom
    try:
        bpy.ops.ed.undo()  # decode raises; must be reported, not crash
    except Exception as ex:
        # Even if the operator surfaces the error, the process must be alive.
        print("NOTE: ed.undo surfaced {!r}".format(ex))
    finally:
        undo_mod.decode = orig_decode
    # Still alive and operable?
    if bpy.data.objects.get(OBJ) is None:
        _fail("object lost after decode exception")
    bpy.ops.ed.undo()  # a real undo still works afterwards
    bpy.ops.ed.redo()
    print("PASS: exception in undo_decode did not crash; undo still works")

    # -- 2. undo after mode exit must not crash ---------------------------
    # Bring history to the top, then exit the mode (frees session + meshlog).
    while OBJ in engine.sessions and session.meshlog_cursor < 2:
        bpy.ops.ed.redo()
    bpy.ops.object.custom_mode_toggle()  # exit
    if OBJ in engine.sessions:
        _fail("session not freed on mode exit")
    # Undo across the exit boundary: decode finds no session -> engine no-op.
    n_before = len(bpy.data.objects[OBJ].data.vertices)
    bpy.ops.ed.undo()
    bpy.ops.ed.undo()
    if bpy.data.objects.get(OBJ) is None:
        _fail("object lost undoing across exit boundary")
    n_after = len(bpy.data.objects[OBJ].data.vertices)
    if n_after != n_before:
        # Not necessarily wrong (memfile could restore), but flag topology change.
        print("NOTE: vertex count changed across exit-boundary undo "
              "({:d} -> {:d})".format(n_before, n_after))
    print("PASS: undo across mode-exit boundary did not crash")

    print("ALL PASS: delta-undo robustness (exception + exit boundary) verified.")


if __name__ == "__main__":
    main()
