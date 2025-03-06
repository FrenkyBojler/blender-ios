# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import unittest
import bpy
import sys
import pathlib

from bpy_extras import anim_utils


"""
blender -b --factory-startup --python tests/python/bl_animation_bake.py -- --testdir tests/data/animation/
"""


class ObjectBakeTest(unittest.TestCase):
    obj: bpy.types.Object

    def setUp(self) -> None:
        bpy.ops.wm.read_homefile(use_factory_startup=True)
        self.obj = bpy.data.objects.new("test_object", None)
        bpy.context.scene.collection.objects.link(self.obj)
        self.obj.animation_data_create()

    def test_bake_without_action(self):
        self.assertEqual(self.obj.animation_data.action, None)
        bake_options = anim_utils.BakeOptions(
            only_selected=False,
            do_pose=False,
            do_object=True,
            do_visual_keying=False,
            do_constraint_clear=False,
            do_parents_clear=False,
            do_clean=False,
            do_location=True,
            do_rotation=True,
            do_scale=True,
            do_bbone=False,
            do_custom_props=False,
        )
        anim_utils.bake_action_objects(((self.obj, None),), frames=range(0, 10), bake_options=bake_options)
        action = self.obj.animation_data.action
        self.assertTrue(action != None, "Baking without an existing action should create an action")
        self.assertEqual(len(action.slots), 1, "Baking should have created a slot")
        self.assertEqual(action.slots[0], self.obj.animation_data.action_slot)
        channelbag = anim_utils.action_get_channelbag_for_slot(action, action.slots[0])
        self.assertTrue(channelbag != None)
        self.assertEqual(len(channelbag.fcurves), 9)
        for fcurve in channelbag.fcurves:
            self.assertEqual(len(fcurve.keyframe_points), 10)
            self.assertEqual(fcurve.keyframe_points[0].co.x, 0)
            self.assertEqual(fcurve.keyframe_points[-1].co.x, 9, "Baking range is exclusive for the end")
        
    
def main():
    global args
    import argparse

    argv = [sys.argv[0]]
    if '--' in sys.argv:
        argv += sys.argv[sys.argv.index('--') + 1:]

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)
    parser.add_argument(
        "--output-dir",
        dest="output_dir",
        type=pathlib.Path,
        default=pathlib.Path("."),
        help="Where to output temp saved blendfiles",
        required=False,
    )

    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()