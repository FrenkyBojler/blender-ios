# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy

import math
import sys
import unittest
import pathlib
import mathutils

"""
blender -b --factory-startup --python tests/python/bl_animation_convert.py -- --testdir tests/files/animation/
"""


def _get_fcurves_with_rna_path(action: bpy.types.Action, slot: bpy.types.ActionSlot,
                               rna_path: str) -> list[bpy.types.FCurve]:
    fcurves = []
    for layer in action.layers:
        for strip in layer.strips:
            channelbag = strip.channelbag(slot)
            if not channelbag:
                continue
            for fcurve in channelbag.fcurves:
                if fcurve.data_path == rna_path:
                    fcurves.append(fcurve)
    return fcurves


def _action_slot_has_rna_path(action: bpy.types.Action, slot: bpy.types.ActionSlot, rna_path: str) -> bool:
    for layer in action.layers:
        for strip in layer.strips:
            channelbag = strip.channelbag(slot)
            if not channelbag:
                continue
            for fcurve in channelbag.fcurves:
                if fcurve.data_path == rna_path:
                    return True
    return False


class ConvertRotationMode(unittest.TestCase):

    action: bpy.types.Action
    action_slot: bpy.types.ActionSlot

    keyed_frames = [1, 6, 11, 16, 21]
    bone_quat: bpy.types.PoseBone
    bone_axis_angle: bpy.types.PoseBone
    bone_xyz: bpy.types.PoseBone
    bone_zyx: bpy.types.PoseBone

    bone_euler_360: bpy.types.PoseBone

    bone_no_rotation_keys: bpy.types.PoseBone
    bone_partially_keyed: bpy.types.PoseBone

    def _assert_almost_equal_rotation_matrix(self, a: mathutils.Matrix, b: mathutils.Matrix):
        equal = True
        for j in range(3):
            for i in range(3):
                if abs(a.row[i][j] - b.row[i][j]) > 0.001:
                    equal = False
                    break
        if not equal:
            raise AssertionError(f"Rotation part of matrices doesn't match\n{a}\n{b}")

    def _assert_almost_equal_euler(self, a: mathutils.Euler, b: mathutils.Euler):
        msg = f"Difference in Euler: {a} - {b}"
        self.assertAlmostEqual(a.x, b.x, 2, msg)
        self.assertAlmostEqual(a.y, b.y, 2, msg)
        self.assertAlmostEqual(a.z, b.z, 2, msg)

    def _assert_almost_equal_quat(self, a: mathutils.Quaternion, b: mathutils.Quaternion):
        msg = f"Difference in Quaternion: {a} - {b}"
        self.assertAlmostEqual(a.x, b.x, 2, msg)
        self.assertAlmostEqual(a.y, b.y, 2, msg)
        self.assertAlmostEqual(a.z, b.z, 2, msg)
        self.assertAlmostEqual(a.w, b.w, 2, msg)

    def setUp(self) -> None:
        bpy.ops.wm.open_mainfile(filepath=str(args.testdir / "rotation_mode_conversion.blend"))
        armature_ob = bpy.data.objects["Armature"]
        self.action = armature_ob.animation_data.action
        self.action_slot = armature_ob.animation_data.action_slot
        pose = armature_ob.pose

        self.bone_quat = pose.bones["bone_quat"]
        self.bone_axis_angle = pose.bones["bone_axis_angle"]
        self.bone_xyz = pose.bones["bone_xyz"]
        self.bone_zyx = pose.bones["bone_zyx"]

        self.bone_euler_360 = pose.bones["bone_euler_rotation_360"]

        self.bone_no_rotation_keys = pose.bones["bone_no_rotation_keys"]
        self.bone_partially_keyed = pose.bones["bone_partially_keyed"]

    def test_convert_quat_to_xyz(self):
        self.bone_quat.convert_rotation_mode('XYZ')
        self.assertEqual(self.bone_quat.rotation_mode, 'XYZ')
        for frame in self.keyed_frames:
            bpy.context.scene.frame_set(frame)
            self._assert_almost_equal_euler(self.bone_quat.rotation_euler, self.bone_xyz.rotation_euler)

    def test_convert_xyz_to_zyx(self):
        self.bone_xyz.convert_rotation_mode('ZYX')
        self.assertEqual(self.bone_xyz.rotation_mode, 'ZYX')
        for frame in self.keyed_frames:
            bpy.context.scene.frame_set(frame)
            self._assert_almost_equal_euler(self.bone_xyz.rotation_euler, self.bone_zyx.rotation_euler)

    def test_convert_axis_angle_to_quat(self):
        self.bone_axis_angle.convert_rotation_mode('QUATERNION')
        self.assertEqual(self.bone_axis_angle.rotation_mode, 'QUATERNION')
        for frame in self.keyed_frames:
            bpy.context.scene.frame_set(frame)
            self._assert_almost_equal_quat(self.bone_axis_angle.rotation_quaternion, self.bone_quat.rotation_quaternion)

    def test_convert_xyz_to_quat(self):
        self.bone_xyz.convert_rotation_mode('QUATERNION')
        self.assertEqual(self.bone_xyz.rotation_mode, 'QUATERNION')
        for frame in self.keyed_frames:
            bpy.context.scene.frame_set(frame)
            self._assert_almost_equal_quat(self.bone_xyz.rotation_quaternion, self.bone_quat.rotation_quaternion)

    def test_convert_360_rotation_limitation(self):
        """ When converting from euler with deltas >180 degrees between keyframes,
        the resulting animation loses that information."""
        bpy.context.scene.frame_set(21)
        self.assertAlmostEqual(self.bone_euler_360.rotation_euler.x, math.radians(360), 2)
        self.assertEqual(self.bone_euler_360.rotation_mode, 'XYZ')
        self.bone_euler_360.convert_rotation_mode('XYZ')
        # Trying to convert to the current rotation mode should be a no op.
        self.assertAlmostEqual(self.bone_euler_360.rotation_euler.x, math.radians(360), 2)

        self.bone_euler_360.convert_rotation_mode('XZY')
        # Ensure depsgraph is evaluated so animation data is refreshed.
        bpy.context.evaluated_depsgraph_get()
        # The information about the 360 degree rotation is lost in this case.
        self.assertAlmostEqual(self.bone_euler_360.rotation_euler.x, math.radians(0), 2)

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
        matrix_before: mathutils.Matrix = self.bone_no_rotation_keys.matrix
        self.assertFalse(
            _action_slot_has_rna_path(
                self.action,
                self.action_slot,
                'pose.bones["bone_no_rotation_keys"].rotation_quaternion'))
        self.bone_no_rotation_keys.convert_rotation_mode('XYZ')
        bpy.context.evaluated_depsgraph_get()
        matrix_after = self.bone_no_rotation_keys.matrix
        self._assert_almost_equal_rotation_matrix(matrix_before, matrix_after)
        self.assertFalse(
            _action_slot_has_rna_path(
                self.action,
                self.action_slot,
                'pose.bones["bone_no_rotation_keys"].rotation_euler'))

    def test_result_is_euler_filtered(self):
        """ When converting rotation modes we should not have sudden 180 degree jumps in euler mode. """
        pass

    def test_convert_partially_keyed_rotation(self):
        """ When converting rotations without baking the resulting animation will have all channels keyed if at least one channel has a key on a frame. """
        fcurves = _get_fcurves_with_rna_path(
            self.action,
            self.action_slot,
            'pose.bones["bone_partially_keyed"].rotation_euler')
        self.assertEqual(len(fcurves), 2)
        self.bone_partially_keyed.convert_rotation_mode('XZY')

        fcurves = _get_fcurves_with_rna_path(
            self.action,
            self.action_slot,
            'pose.bones["bone_partially_keyed"].rotation_euler')
        self.assertEqual(len(fcurves), 3)
        expected_frames = [1, 6, 16, 21]
        for fcurve in fcurves:
            for i, frame in enumerate(expected_frames):
                self.assertAlmostEqual(fcurve.keyframe_points[i].co[0], frame, 2)

    def test_convert_subframes(self):
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
