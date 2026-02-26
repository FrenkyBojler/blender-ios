# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
import sys
import unittest
import pathlib

"""
blender -b --factory-startup --python tests/python/bl_animation_convert.py -- --testdir tests/files/animation/
"""

class ConvertRotationMode(unittest.TestCase):

    bone_quat: bpy.types.PoseBone
    bone_axis_angle: bpy.types.PoseBone
    bone_xyz: bpy.types.PoseBone
    bone_zyx: bpy.types.PoseBone

    bone_euler_360: bpy.types.PoseBone
    bone_euler_720: bpy.types.PoseBone

    bone_no_rotation_keys: bpy.types.PoseBone

    def setUp(self) -> None:
        bpy.ops.wm.open_mainfile(filepath=str(args.testdir / "rotation_mode_conversion.blend"))
        pose = bpy.data.objects["Armature"].pose
        self.bone_quat = pose.bones["bone_quat"]
        self.bone_axis_angle = pose.bones["bone_axis_angle"]
        self.bone_xyz = pose.bones["bone_xyz"]
        self.bone_zyx = pose.bones["bone_zyx"]

        self.bone_euler_360 = pose.bones["bone_euler_rotation_360"]
        self.bone_euler_720 = pose.bones["bone_euler_rotation_720"]

        self.bone_no_rotation_keys = pose.bones["bone_no_rotation_keys"]
    
    def test_convert_quat_to_euler(self):
        keyed_frames = [1, 6, 11, 16, 21]
        self.bone_quat.convert_rotation_mode("XYZ")

    def test_convert_360_rotation_limitation(self):
        """ When converting from euler with deltas >180 degrees between keyframes, 
        the resulting animation loses that information."""
        pass

    def test_convert_360_rotation_bake(self):
        """ When converting rotations >180 degrees baking has to 
        be used to ensure the interpolation is preserved. """
        pass

    def test_convert_keyed_rotation_mode(self):
        """ When the rotation mode itself is keyed and changes during the animation, 
        the conversion has to make sure the animation is preserved. """
        pass

    def test_convert_unkeyed_rotation(self):
        """ When converting the rotation mode on a bone that has no keys on its rotation channels,
         the function should just change the rotation mode while preserving the visual rotation. """
        pass


def main():
    global args
    import argparse

    argv = [sys.argv[0]]
    if '--' in sys.argv:
        argv += sys.argv[sys.argv.index('--') + 1:]

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)

    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
