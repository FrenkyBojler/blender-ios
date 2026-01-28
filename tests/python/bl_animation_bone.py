# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later


import unittest
import bpy
import pathlib
import sys


_BONE_NAME_A = "bone_a"
_BONE_NAME_B = "bone_b"


def _set_up_driver(driver, target_id, target_path):
    driver_variable_name = "var"

    driver.type = 'AVERAGE'
    driver.expression = driver_variable_name

    driver_var = driver.variables.new()
    driver_var.name = driver_variable_name
    driver_var.type = 'SINGLE_PROP'

    driver_target = driver_var.targets[0]
    driver_target.id_type = 'OBJECT'
    driver_target.id = target_id
    driver_target.data_path = target_path


class PoseBoneRenameTest(unittest.TestCase):
    """Ensure that drivers and animation are moved to the correct RNA path once a pose bone is renamed."""

    def setUp(self) -> None:
        super().setUp()
        bpy.ops.wm.read_homefile(use_factory_startup=True)

        armature = bpy.data.armatures.new("test_armature")
        self.armature_obj = bpy.data.objects.new("test_armature_object", armature)
        bpy.context.scene.collection.objects.link(self.armature_obj)
        bpy.context.view_layer.objects.active = self.armature_obj
        self.armature_obj.select_set(True)

        bpy.ops.object.mode_set(mode='EDIT')
        edit_bone = armature.edit_bones.new(_BONE_NAME_A)
        edit_bone.head = (1, 0, 0)
        edit_bone.tail = (0, 0, 0)

        edit_bone = armature.edit_bones.new(_BONE_NAME_B)
        edit_bone.head = (1, 1, 0)
        edit_bone.tail = (0, 1, 0)
        bpy.ops.object.mode_set(mode='OBJECT')

    def test_rename_bone_driver(self):
        bpy.ops.object.mode_set(mode='OBJECT')
        fcu = self.armature_obj.data.bones[_BONE_NAME_A].driver_add(f"bbone_segments", -1)
        self.assertEqual(fcu.data_path, f"bones[\"{_BONE_NAME_A}\"].bbone_segments")
        driver = fcu.driver

        _set_up_driver(driver, self.armature_obj, f"pose.bones[\"{_BONE_NAME_B}\"].location[0]")

        bone_a_rename = "bone_a_2"
        self.armature_obj.pose.bones[_BONE_NAME_A].name = bone_a_rename
        bpy.context.view_layer.update()

        # `bbone_segments` is a property of the bone thus armature.
        self.assertEqual(len(self.armature_obj.data.animation_data.drivers), 1, "Shouldn't remove the driver")
        self.assertEqual(fcu.data_path, f"bones[\"{bone_a_rename}\"].bbone_segments")

        bone_b_rename = "bone_b_2"
        self.armature_obj.pose.bones[_BONE_NAME_B].name = bone_b_rename

        # Testing if driver targets are still correct after a rename.
        # TODO: this currently does not work!
        # driver_target = driver.variables[0].targets[0]
        # self.assertEqual(driver_target.data_path, f"pose.bones[\"{bone_b_rename}\"].location[0]")

    def test_rename_pose_bone_driver(self):
        bpy.ops.object.mode_set(mode='OBJECT')
        fcu = self.armature_obj.pose.bones[_BONE_NAME_A].driver_add("hide", -1)
        self.assertEqual(fcu.data_path, f"pose.bones[\"{_BONE_NAME_A}\"].hide")
        driver = fcu.driver

        _set_up_driver(driver, self.armature_obj, f"pose.bones[\"{_BONE_NAME_B}\"].location[0]")

        bone_a_rename = "bone_a_2"
        self.armature_obj.pose.bones[_BONE_NAME_A].name = bone_a_rename
        bpy.context.view_layer.update()

        # `hide` is a property on the pose bone thus object.
        self.assertEqual(len(self.armature_obj.animation_data.drivers), 1, "Shouldn't remove the driver")
        self.assertEqual(fcu.data_path, f"pose.bones[\"{bone_a_rename}\"].hide")

        bone_b_rename = "bone_b_2"
        self.armature_obj.pose.bones[_BONE_NAME_B].name = bone_b_rename

        # Testing if driver targets are still correct after a rename.
        # TODO: this currently does not work!
        # driver_target = driver.variables[0].targets[0]
        # self.assertEqual(driver_target.data_path, f"pose.bones[\"{bone_b_rename}\"].location[0]")


def main():
    global args
    import argparse

    argv = [sys.argv[0]]
    if '--' in sys.argv:
        argv += sys.argv[sys.argv.index('--') + 1:]

    parser = argparse.ArgumentParser()
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
