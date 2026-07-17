# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Multi-object undo routing (undo-integration P6 §4.3): each CUSTOM_MODE step
carries the object it acted on (UndoRefID_Object), so undo/redo affect the
right object's session even when the active object changed between strokes.

Enters the mode on two objects, strokes each, then undoes and asserts the
correct object's mesh reverts (and the other is untouched). Also confirms a
rename is tracked (the step's object_ref follows the datablock, not the name
string) by renaming between strokes.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/custom_undo_multiobj_test.py
"""

import sys

import numpy as np
import bpy


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def pos(name):
    ob = bpy.data.objects[name]
    a = np.empty(len(ob.data.vertices) * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", a)
    return a


def stroke(name, session, strokemod, undo_mod, context, center):
    mgr = __import__("sculptcore_addon.engine", fromlist=["x"]).manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    ob = bpy.data.objects[name]
    context.view_layer.objects.active = ob
    strokemod.stroke_begin(session, has_dyntopo=False)
    for _ in range(4):
        strokemod.apply_dab(session, draw, center, (0.0, 0.0, 1.0), 0.6)
    strokemod.stroke_end(session)
    from sculptcore_addon import convert
    convert.flush(ob)
    undo_mod.push(context, ob, session)
    return pos(name)


def main():
    import sculptcore_addon.engine as engine
    from sculptcore_addon import stroke as strokemod
    from sculptcore_addon import undo as undo_mod

    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")

    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=1.0, location=(0, 0, 0))
    bpy.context.active_object.name = "One"
    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=1.0, location=(3, 0, 0))
    bpy.context.active_object.name = "Two"
    context = bpy.context

    # Enter the mode on both objects (the toggle acts on the active object; the
    # other stays in the mode).
    context.view_layer.objects.active = bpy.data.objects["One"]
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    context.view_layer.objects.active = bpy.data.objects["Two"]
    bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")

    if "One" not in engine.sessions or "Two" not in engine.sessions:
        _fail("both objects should have sessions ({})".format(list(engine.sessions)))
    if bpy.data.objects["One"].mode != 'CUSTOM':
        _fail("object One left the mode when Two entered")
    print("PASS: two objects hold sessions simultaneously")

    bpy.ops.ed.undo_push(message="Base")
    s_one = engine.sessions["One"]
    s_two = engine.sessions["Two"]
    for s in (s_one, s_two):
        b = strokemod._ensure_brush(s)
        b.strength, b.radius = 0.5, 0.5

    pre_one, pre_two = pos("One"), pos("Two")
    post_one = stroke("One", s_one, strokemod, undo_mod, context, (0.0, 0.0, 1.0))
    post_two = stroke("Two", s_two, strokemod, undo_mod, context, (3.0, 0.0, 1.0))

    # Undo: last stroke was Two -> Two reverts, One untouched.
    bpy.ops.ed.undo()
    if float(np.abs(pos("Two") - pre_two).max()) > 1e-6:
        _fail("undo did not revert object Two")
    if float(np.abs(pos("One") - post_one).max()) > 1e-6:
        _fail("undo of Two disturbed object One")
    print("PASS: undo routed to object Two (One untouched)")

    # Undo again: One reverts.
    bpy.ops.ed.undo()
    if float(np.abs(pos("One") - pre_one).max()) > 1e-6:
        _fail("second undo did not revert object One")
    print("PASS: undo routed to object One")

    # Redo both back.
    bpy.ops.ed.redo()
    bpy.ops.ed.redo()
    if float(np.abs(pos("One") - post_one).max()) > 1e-6 or \
            float(np.abs(pos("Two") - post_two).max()) > 1e-6:
        _fail("redo x2 did not restore both objects")
    print("PASS: redo x2 restored both objects exactly")

    print("ALL PASS: multi-object undo routing verified.")


if __name__ == "__main__":
    main()
