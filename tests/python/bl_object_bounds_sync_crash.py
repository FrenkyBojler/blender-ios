# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Regression test for #122451: crash on animated viewport visibility + animated mesh.

Scene setup (built on the fly, no committed .blend file)
========================================================

* A mesh object with **animated viewport visibility** (``hide_viewport`` keyed on the
  object action).
* **Animated mesh geometry** (``vertices[0].co`` keyed on a separate mesh action).
  Object and mesh must use **separate slotted actions**; sharing one action does not
  reproduce the bug.
* A **save / reload** round-trip. The crash does not reproduce from an in-memory setup
  alone; reloading the file leaves depsgraph state that matches the original report.
* Scrubbing the timeline from frame **72** to **71** twice with depsgraph updates,
  matching UI steps from the bug report (drag playhead back, then interact again).
"""

import tempfile
import unittest

import bpy
from mathutils import Vector

# Quad from the simplified project file attached to #122451. Non-axis-aligned geometry
# stresses evaluated bounds; a default plane with the same animation setup does not
# always reproduce the crash after save/reload.
_CRASH_MESH_VERTS = (
    (0.0, 1.552757978439331, -5.602549076080322),
    (5.0, 0.10000000149011612, 1.0),
    (0.3425966799259186, -1.552757978439331, 5.602549076080322),
    (-5.0, -0.10000000149011612, -1.0),
)


def build_crash_scene():
    """
    Build a minimal #122451 scene: animated visibility + animated mesh verts.

    Returns the object. Caller should save/reload before scrubbing the timeline.
    """
    mesh = bpy.data.meshes.new("TestMesh")
    mesh.from_pydata(
        [Vector(co) for co in _CRASH_MESH_VERTS],
        [(0, 1), (1, 2), (2, 3), (3, 0)],
        [(0, 1, 2, 3)],
    )
    mesh.update()

    ob = bpy.data.objects.new("TestObject", mesh)
    bpy.context.scene.collection.objects.link(ob)

    # Object action: viewport visibility (hidden at frame 0, visible at frame 72).
    ob.animation_data_create()
    ob_action = bpy.data.actions.new("ObjectAction")
    ob_slot = ob_action.slots.new("OBJECT", "ObjectSlot")
    ob_cb = ob_action.layers.new("Layer").strips.new(type="KEYFRAME").channelbags.new(ob_slot)
    hide_fcurve = ob_cb.fcurves.new(data_path="hide_viewport", index=0)
    hide_fcurve.keyframe_points.insert(0, 1.0).interpolation = "CONSTANT"
    hide_fcurve.keyframe_points.insert(72, 0.0).interpolation = "CONSTANT"
    ob.animation_data.action = ob_action
    ob.animation_data.action_slot = ob_slot

    # Mesh action: vertex animation on a separate action (required for repro).
    mesh.animation_data_create()
    me_action = bpy.data.actions.new("MeshAction")
    me_slot = me_action.slots.new("MESH", "MeshSlot")
    me_cb = me_action.layers.new("Layer").strips.new(type="KEYFRAME").channelbags.new(me_slot)
    vert_fcurve = me_cb.fcurves.new(data_path="vertices[0].co", index=0)
    vert_fcurve.keyframe_points.insert(168, 0.0)
    vert_fcurve.keyframe_points.insert(192, -0.3425966799259186)
    mesh.animation_data.action = me_action
    mesh.animation_data.action_slot = me_slot

    scene = bpy.context.scene
    scene.frame_start = 1
    scene.frame_end = 193
    scene.frame_set(72)

    return ob


def scrub_timeline_from_72_to_71():
    """Reproduce timeline scrub from the #122451 report (frame 72 -> 71, twice)."""
    scene = bpy.context.scene
    scene.frame_set(71)
    bpy.context.view_layer.update()
    scene.frame_set(71)
    bpy.context.view_layer.update()


class ObjectBoundsSyncCrashTest(unittest.TestCase):
    """#122451: bounds sync in ``sync_to_original`` must not read stale eval geometry."""

    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_timeline_scrub_with_separate_actions(self):
        """
        Scrubbing after save/reload must not crash when reading ``dimensions``.

        Exercises depsgraph evaluation with animated visibility and mesh data, then
        checks that RNA can read evaluated bounds on the original object.
        """
        ob = build_crash_scene()
        self.assertIsNot(
            ob.animation_data.action,
            ob.data.animation_data.action,
            "Object and mesh must use separate actions for this repro",
        )

        with tempfile.NamedTemporaryFile(suffix=".blend") as temp_blend:
            bpy.ops.wm.save_as_mainfile(filepath=temp_blend.name)
            bpy.ops.wm.open_mainfile(filepath=temp_blend.name, load_ui=False)

        self.assertEqual(bpy.context.scene.frame_current, 72)
        scrub_timeline_from_72_to_71()

        ob = bpy.data.objects["TestObject"]
        dimensions = ob.dimensions
        self.assertGreater(dimensions[0], 0.0)
        self.assertEqual(bpy.context.scene.frame_current, 71)


if __name__ == "__main__":
    import sys

    sys.argv = [sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()
