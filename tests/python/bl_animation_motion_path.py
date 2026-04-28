# SPDX-FileCopyrightText: 2020-2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import unittest
import bpy
import sys
import pathlib


class MotionPathTestObject(unittest.TestCase):

    anim_object: bpy.types.Object

    def setUp(self) -> None:
        super().setUp()
        bpy.ops.wm.read_homefile(use_factory_startup=True)
        self.anim_object = bpy.data.objects.new("anim_object", None)
        bpy.context.scene.collection.objects.link(self.anim_object)
        bpy.context.view_layer.objects.active = self.anim_object
        self.anim_object.select_set(True)

    def test_cache_range(self):
        """Testing if the motion path creates the correct range."""
        self.anim_object.keyframe_insert("location", frame=0)
        self.anim_object.keyframe_insert("location", frame=10)

        bpy.ops.object.paths_calculate(range='SCENE')
        self.assertNotEqual(self.anim_object.motion_path, None)
        motion_path = self.anim_object.motion_path
        self.assertEqual(motion_path.frame_start, bpy.context.scene.frame_start)
        # The motion path frame_end is exclusive while the scene frame_end is inclusive.
        self.assertEqual(motion_path.frame_end, bpy.context.scene.frame_end + 1)

        bpy.ops.object.paths_calculate(range='KEYS_ALL')
        self.assertEqual(motion_path.frame_start, 0)
        self.assertEqual(motion_path.frame_end, 11)

        bpy.ops.object.paths_calculate(range='KEYS_SELECTED')
        # Both keys are selected.
        self.assertEqual(motion_path.frame_start, 0)
        self.assertEqual(motion_path.frame_end, 11)

        self.anim_object.animation_visualization.motion_path.frame_start = 3
        # frame_end is inclusive.
        self.anim_object.animation_visualization.motion_path.frame_end = 6
        bpy.ops.object.paths_calculate(range='MANUAL')
        self.assertEqual(motion_path.frame_start, 3)
        self.assertEqual(motion_path.frame_end, 7)


class MotionPathTestArmature(unittest.TestCase):
    anim_armature_object: bpy.types.Object
    pose_bone_a: bpy.types.PoseBone
    pose_bone_b: bpy.types.PoseBone

    def setUp(self) -> None:
        super().setUp()
        bpy.ops.wm.read_homefile(use_factory_startup=True)
        armature = bpy.data.armatures.new("anim_armature")
        self.anim_armature_object = bpy.data.objects.new("anim_armature_ob", armature)
        bpy.context.scene.collection.objects.link(self.anim_armature_object)
        bpy.context.view_layer.objects.active = self.anim_armature_object
        self.anim_armature_object.select_set(True)

        bone_name_a = "bone"
        bone_name_b = "bone_2"
        bpy.ops.object.mode_set(mode='EDIT')
        edit_bone = armature.edit_bones.new(bone_name_a)
        edit_bone.head = (1, 0, 0)
        edit_bone = armature.edit_bones.new(bone_name_b)
        edit_bone.head = (1, 1, 0)
        edit_bone.tail = (0, 1, 0)

        bpy.ops.object.mode_set(mode='POSE')
        self.pose_bone_a = self.anim_armature_object.pose.bones[bone_name_a]
        self.pose_bone_a.select = True
        self.pose_bone_b = self.anim_armature_object.pose.bones[bone_name_b]
        # Second bone is not selected by default. Should get no motion path.
        self.pose_bone_b.select = False

    def test_cache_range(self):
        """Teting if the motion path creates the correct range."""
        self.pose_bone_a.keyframe_insert("location", frame=0)
        self.pose_bone_a.keyframe_insert("location", frame=10)

        bpy.ops.pose.paths_calculate(range='SCENE')
        self.assertNotEqual(self.pose_bone_a.motion_path, None)
        self.assertEqual(
            self.pose_bone_b.motion_path,
            None,
            "The unselected bone should have no motion path calculated.")
        motion_path = self.pose_bone_a.motion_path

        self.assertEqual(motion_path.frame_start, bpy.context.scene.frame_start)
        # The motion path frame_end is exclusive while the scene frame_end is inclusive.
        self.assertEqual(motion_path.frame_end, bpy.context.scene.frame_end + 1)

    def test_bake_location(self):
        pass


def main():
    global args
    import argparse

    if '--' in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
