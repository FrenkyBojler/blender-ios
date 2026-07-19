# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Foreign-memfile interleave (undo-integration P6 §4.1 / §6): a memfile step
(e.g. a scene tweak) pushed between sculpt strokes must not corrupt delta undo.

Stack built here: base(mf), A(c), B(c), foreign(mf), C(c). A mesh-preserving
foreign memfile step does not change the mesh, so the seek-based decode keeps
the engine in sync and the memfile decodes restore the same state. Asserts the
mesh is exact at every stop of undo x4 then redo x4.

Run:
    set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
    blender --background --factory-startup --python claudeMemory/tests/custom_undo_interleave_test.py
"""

import sys

import numpy as np
import bpy

OBJ = "Inter"


def _fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def positions():
    ob = bpy.data.objects[OBJ]
    a = np.empty(len(ob.data.vertices) * 3, dtype=np.float32)
    ob.data.vertices.foreach_get("co", a)
    return a


def assert_pos(label, expected):
    d = float(np.abs(positions() - expected).max())
    if d > 1e-6:
        _fail("{:s}: differ by {:.3e}".format(label, d))
    print("PASS:", label)


def do_stroke(session, strokemod, undo_mod, context, center):
    mgr = __import__("sculptcore_addon.engine", fromlist=["x"]).manager()
    draw = int(mgr.get("sculptcore::brush::SculptBrushes").items["DRAW"])
    ob = bpy.data.objects[OBJ]
    strokemod.stroke_begin(session, has_dyntopo=False)
    for _ in range(4):
        strokemod.apply_dab(session, draw, center, (0.0, 0.0, 1.0), 0.6)
    strokemod.stroke_end(session)
    from sculptcore_addon import convert
    convert.flush(ob)
    undo_mod.push(context, ob, session)
    return positions()


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
    brush = strokemod._ensure_brush(session)
    brush.strength, brush.radius = 0.5, 0.5
    brush.writeProps()

    pre = positions()
    post_a = do_stroke(session, strokemod, undo_mod, context, (0.0, 0.0, 1.0))
    if not (np.isfinite(post_a).all() and np.abs(post_a - pre).max() > 1e-6):
        _fail("stroke A did not change positions (brush setup?)")
    post_b = do_stroke(session, strokemod, undo_mod, context, (1.0, 0.0, 0.0))
    # Foreign memfile step between strokes (a mesh-preserving snapshot, e.g. a
    # scene-setting tweak the user makes mid-sculpt).
    context.scene.frame_current += 1
    bpy.ops.ed.undo_push(message="Foreign scene tweak")
    post_c = do_stroke(session, strokemod, undo_mod, context, (0.0, 1.0, 0.0))

    # undo x4: C -> foreign(postB) -> B(postB) -> A(postA) -> base(pre).
    bpy.ops.ed.undo()
    assert_pos("undo C -> foreign (postB)", post_b)
    bpy.ops.ed.undo()
    assert_pos("undo foreign -> B (postB)", post_b)
    bpy.ops.ed.undo()
    assert_pos("undo B -> A (postA)", post_a)
    bpy.ops.ed.undo()
    assert_pos("undo A -> base (pre)", pre)

    # redo x4 back up to C.
    bpy.ops.ed.redo()
    assert_pos("redo base -> A (postA)", post_a)
    bpy.ops.ed.redo()
    assert_pos("redo A -> B (postB)", post_b)
    bpy.ops.ed.redo()
    assert_pos("redo B -> foreign (postB)", post_b)
    bpy.ops.ed.redo()
    assert_pos("redo foreign -> C (postC)", post_c)

    print("ALL PASS: foreign-memfile interleave coherent (mesh-preserving).")


if __name__ == "__main__":
    main()
