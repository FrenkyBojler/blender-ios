# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Undo memory-limit verification (undo-integration P6 B4 / §6: "200 strokes with
a small undo limit -> memory stays bounded, oldest steps evicted, no crash").

Each CUSTOM_MODE step reports a truthful byte size (meshlog stepMemSize), so
Blender's own undo memory limiter evicts old steps once the stack exceeds the
preference; eviction calls step_free -> undo_free -> meshlog.freeStep, so the
engine reclaims the memory too. This drives many strokes under a small limit
and asserts the meshlog's retained memory stays bounded and undo still works.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/custom_undo_memory_test.py
"""

import sys

import bpy

OBJ = "Memtest"
N_STROKES = 120


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod
    from sculptcore_addon import convert

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")

    # A small undo memory limit so eviction kicks in within N_STROKES.
    prefs = bpy.context.preferences.edit
    prefs.use_global_undo = True
    prefs.undo_memory_limit = 2  # MB
    prefs.undo_steps = 256

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=1.0)
    bpy.context.active_object.name = OBJ
    context = bpy.context

    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    bpy.ops.ed.undo_push(message="Base")
    session = engine.sessions[OBJ]
    mgr = engine.manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius = 0.4, 0.4

    ob = bpy.data.objects[OBJ]
    pushed_total = 0.0
    for i in range(N_STROKES):
        c = ((i % 5) * 0.2 - 0.4, ((i // 5) % 5) * 0.2 - 0.4, 1.0)
        strokemod.stroke_begin(session, has_dyntopo=False)
        for _ in range(4):
            strokemod.apply_dab(session, draw, c, (0.0, 0.0, 1.0), 0.5)
        strokemod.stroke_end(session)
        convert.flush(ob)
        undo_mod.push(context, ob, session)
        pushed_total += float(session.meshlog.stepMemSize(int(session.meshlog.lastStepId())))

    retained = float(session.meshlog.totalMemSize())
    print("PASS: {:d} strokes; pushed ~{:.1f}MB total, meshlog retains ~{:.1f}MB "
          "(limit {:d}MB)".format(N_STROKES, pushed_total / 1e6, retained / 1e6,
                                  prefs.undo_memory_limit))

    # Eviction (Blender limiter -> step_free -> freeStep) must keep the retained
    # engine memory well below everything pushed, roughly tracking the limit.
    if retained >= pushed_total * 0.7:
        _fail("meshlog memory not bounded: retains {:.1f}MB of {:.1f}MB pushed "
              "(eviction/free not firing)".format(retained / 1e6, pushed_total / 1e6))
    print("PASS: memory bounded ({:.1f}MB retained of {:.1f}MB pushed)".format(
        retained / 1e6, pushed_total / 1e6))

    # Undo a handful of times into the retained horizon — must not crash.
    for _ in range(5):
        bpy.ops.ed.undo()
    print("PASS: undo into the retained horizon did not crash")

    print("ALL PASS: undo memory limit / eviction verified.")


if __name__ == "__main__":
    main()
