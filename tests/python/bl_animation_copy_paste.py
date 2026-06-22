# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import bpy
import pathlib
import tempfile
import mathutils

import unittest
import sys


TEST_FILE = "world_space_copy_paste.blend"
COPYBUFFER_NAME = "world_space_buffer.blend"


class AbstractCopyPasteTest(unittest.TestCase):

    def setUp(self) -> None:
        bpy.ops.wm.open_mainfile(filepath=str(args.testdir / TEST_FILE))
        for obj in bpy.data.objects:
            obj.select_set(False)
        # The copybuffer is stored in whatever is returned from `BKE_tempdir_base`.
        # By ensuring that the user pref is empty the used path should always be the
        # system temporary directory.
        bpy.context.preferences.filepaths.temporary_directory = ""
        self._copybuffer_path = pathlib.Path(tempfile.gettempdir()) / COPYBUFFER_NAME


class WorldSpaceCopyTest(AbstractCopyPasteTest):
    """
    Test that copying creates the expected temp file and confirming
    that temp file contains the correct data.
    """

    def tearDown(self) -> None:
        if self._copybuffer_path.exists():
            os.remove(self._copybuffer_path)

    def test_invalid_range(self) -> None:
        obj = bpy.data.objects["armature_simple"]
        obj.select_set(True)
        # Passing an invalid frame range will error.
        with self.assertRaises(RuntimeError):
            bpy.ops.anim.world_space_copy(start=10, end=0)
        with self.assertRaises(RuntimeError):
            bpy.ops.anim.world_space_copy(start=1, end=1)
        self.assertFalse(self._copybuffer_path.exists())

    def _test_for_single_entity_in_buffer(self, entity_name):
        # Asserts that there are only FCurves with matching rna paths in the buffer.
        bpy.ops.wm.open_mainfile(filepath=self._copybuffer_path.as_posix())
        self.assertEqual(len(bpy.data.actions), 1)
        buffer_action = bpy.data.actions[0]
        buffer_fcurves = buffer_action.layers[0].strips[0].channelbags[0].fcurves
        # A matrix is stored in 12 FCurves. The last row of the 4x4 matrix is assumed
        # to always be 0/0/0/1.
        self.assertEqual(len(buffer_fcurves), 12)
        for fcurve in buffer_fcurves:
            self.assertEqual(fcurve.data_path, entity_name)
            # Data is not stored in BezTriple keyframes.
            self.assertEqual(len(fcurve.keyframe_points), 0)
            self.assertEqual(len(fcurve.sampled_points), 10)
            self.assertEqual(fcurve.sampled_points[0].co.x, 0)
            # Range is exclusive at the end so 10 is not copied.
            self.assertEqual(fcurve.sampled_points[-1].co.x, 9)

    def test_copy_object(self) -> None:
        obj = bpy.data.objects["armature_simple"]
        obj_name = obj.name
        obj.select_set(True)
        bpy.ops.anim.world_space_copy(start=0, end=10)
        self.assertTrue(self._copybuffer_path.exists())

        self._test_for_single_entity_in_buffer(obj_name)

    def test_copy_pose_bone(self) -> None:
        obj: bpy.types.Object = bpy.data.objects["armature_simple"]
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode='POSE')
        pose_bone: bpy.types.PoseBone = obj.pose.bones[0]
        pose_bone.select = True
        bpy.ops.anim.world_space_copy(start=0, end=10)
        self.assertTrue(self._copybuffer_path.exists())

        self._test_for_single_entity_in_buffer(pose_bone.name)

    def test_copy_object_no_anim(self) -> None:
        """The copying is done even for objects that are not animated."""
        obj: bpy.types.Object = bpy.data.objects["armature_no_anim"]
        obj.select_set(True)
        obj_name = obj.name
        bpy.ops.anim.world_space_copy(start=0, end=10)
        self.assertTrue(self._copybuffer_path.exists())
        bpy.ops.wm.open_mainfile(filepath=self._copybuffer_path.as_posix())
        buffer_action = bpy.data.actions[0]
        buffer_fcurves = buffer_action.layers[0].strips[0].channelbags[0].fcurves
        for fcurve in buffer_fcurves:
            self.assertEqual(fcurve.data_path, obj_name)
            self.assertEqual(len(fcurve.sampled_points), 10)
            # All the FCurves will be flat.
            self.assertEqual(fcurve.sampled_points[0].co.y, fcurve.sampled_points[-1].co.y)


class WorldSpacePasteTest(AbstractCopyPasteTest):

    def _assert_almost_equal_matrix(self, a: mathutils.Matrix, b: mathutils.Matrix):
        equal = True
        for j in range(4):
            for i in range(4):
                if abs(a.row[i][j] - b.row[i][j]) > 0.001:
                    equal = False
                    break
        if not equal:
            raise AssertionError(f"Matrices don't match\n{a}\n{b}")

    def _assert_bones_equal_world_space(
            self,
            arm_a: bpy.types.Object,
            bone_a: bpy.types.PoseBone,
            arm_b: bpy.types.Object,
            bone_b: bpy.types.PoseBone) -> None:
        for frame in range(10):
            bpy.context.scene.frame_set(frame)
            self._assert_almost_equal_matrix(arm_a.matrix_world @ bone_a.matrix, arm_b.matrix_world @ bone_b.matrix)

    def _assert_objects_equal_world_space(self, obj_a: bpy.types.Object, obj_b: bpy.types.Object) -> None:
        for frame in range(10):
            bpy.context.scene.frame_set(frame)
            self._assert_almost_equal_matrix(obj_a.matrix_world, obj_b.matrix_world)

    def test_paste_to_different_object(self) -> None:
        """Tests that pasting to a differently named object works in the simple 1:1 case."""
        copy_obj: bpy.types.Object = bpy.data.objects["armature_simple"]
        bpy.context.view_layer.objects.active = copy_obj
        paste_obj: bpy.types.Object = bpy.data.objects["paste_armature_single_bone"]
        copy_obj.select_set(True)
        paste_obj.select_set(False)

        bpy.ops.anim.world_space_copy(start=0, end=10)

        copy_obj.select_set(False)
        paste_obj.select_set(True)

        bpy.ops.anim.world_space_paste()

        self._assert_objects_equal_world_space(copy_obj, paste_obj)

    def test_paste_pose_bone(self) -> None:
        """Tests that pasting to equally named bones in different armatures works."""
        copy_obj: bpy.types.Object = bpy.data.objects["armature_simple"]
        copy_obj.select_set(True)
        bpy.context.view_layer.objects.active = copy_obj
        paste_obj: bpy.types.Object = bpy.data.objects["paste_armature_single_bone"]
        paste_obj.select_set(True)

        bpy.ops.object.mode_set(mode='POSE')
        copy_bone: bpy.types.PoseBone = copy_obj.pose.bones[0]
        paste_bone: bpy.types.PoseBone = paste_obj.pose.bones[0]
        copy_bone.select = True
        paste_bone.select = False
        bpy.ops.anim.world_space_copy(start=0, end=10)

        copy_bone.select = False
        paste_bone.select = True
        bpy.ops.anim.world_space_paste()

        self._assert_bones_equal_world_space(copy_obj, copy_bone, paste_obj, paste_bone)

    def test_paste_scale_animation(self) -> None:
        """Pasting an animation of non uniform scale values should work."""
        pass

    def test_indirect_animation(self) -> None:
        # The entity from which we copy may not be animated directly,
        # copying the world space movement should still work.
        pass

    def test_from_objects_to_bones(self) -> None:
        # As long as the names match, this will work.
        pass

    def test_from_single_to_multiple(self) -> None:
        pass

    def test_paste_to_skewed_space(self) -> None:
        """When the space into which we scale is skewed the result may not match 100%."""
        copy_obj: bpy.types.Object = bpy.data.objects["armature_simple"]
        copy_obj.select_set(True)
        bpy.context.view_layer.objects.active = copy_obj
        paste_obj: bpy.types.Object = bpy.data.objects["armature_skewed_space"]
        paste_obj.select_set(False)

        bpy.ops.object.mode_set(mode='POSE')
        copy_bone: bpy.types.PoseBone = copy_obj.pose.bones[0]
        paste_bone: bpy.types.PoseBone = paste_obj.pose.bones[2]
        copy_bone.select = True
        paste_bone.select = False
        bpy.ops.anim.world_space_copy(start=0, end=10)

        for frame in range(10):
            bpy.context.scene.frame_set(frame)
            # TODO


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
