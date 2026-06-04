# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_dynamic_overrides.py -- --output-dir=/tmp/
import pathlib
import bpy
import sys
import os

sys.path.append(os.path.dirname(os.path.realpath(__file__)))
from bl_blendfile_utils import TestHelper


class TestDynamicOverrides(TestHelper):
    MESH_LIBRARY_PARENT = "LibMeshParent"
    OBJECT_LIBRARY_PARENT = "LibMeshParent"
    DYNAMIC_OVERRIDE_SCENE = "DynOverrideScene"

    def __init__(self, args):
        super().__init__(args)

        output_dir = pathlib.Path(self.args.output_dir)
        self.ensure_path(str(output_dir))
        self.output_path = output_dir / "dynamic_overrides_lib.blend"
        self.test_output_path = output_dir / "dynamic_overrides_test.blend"

        bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)
        mesh = bpy.data.meshes.new(TestDynamicOverrides.MESH_LIBRARY_PARENT)
        obj = bpy.data.objects.new(TestDynamicOverrides.OBJECT_LIBRARY_PARENT, object_data=mesh)
        bpy.context.collection.objects.link(obj)

        bpy.ops.wm.save_as_mainfile(filepath=str(self.output_path), check_existing=False, compress=False)

    def test_dynamic_override_local(self):
        bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)
        bpy.data.orphans_purge()

        link_dir = self.output_path / "Object"
        bpy.ops.wm.append(directory=str(link_dir), filename=TestDynamicOverrides.OBJECT_LIBRARY_PARENT)

        obj = bpy.data.objects[TestDynamicOverrides.OBJECT_LIBRARY_PARENT]
        self.assertIsNone(obj.override_library)

        dynoverride = bpy.data.dynamic_overrides.new(TestDynamicOverrides.DYNAMIC_OVERRIDE_SCENE)
        bpy.context.scene.dynamic_override = dynoverride

        obj_dynoverride_rule = dynoverride.rules.ensure_for_iddata(obj)
        # No properties defined yet in this rule, so it does not have any custom RuntimeRNA,
        # just the 'parent' empty type.
        self.assertEqual(obj_dynoverride_rule.override_values.bl_rna.identifier,
                         'DynamicOverrideRuleIDDataOverrideValues')
        self.assertEqual(obj_dynoverride_rule.original_values.bl_rna.identifier,
                         'DynamicOverrideRuleIDDataOriginalValues')
        # Only one property, `rna_type`.
        self.assertEqual(len(obj_dynoverride_rule.override_values.bl_rna.properties), 1)
        self.assertEqual(len(obj_dynoverride_rule.original_values.bl_rna.properties), 1)

        # This will generate a dedicated runtime RNA type (struct definition).
        obj_dynoverride_rule.properties.add_for_rnapath("location")

        self.assertEqual(obj_dynoverride_rule.override_values.bl_rna.identifier,
                         'DynamicOverrideRuleIDDataOverrideValuesRT')
        self.assertEqual(obj_dynoverride_rule.original_values.bl_rna.identifier,
                         'DynamicOverrideRuleIDDataOriginalValuesRT')
        # Now there is the standard `rna_type`, and a `location` property.
        self.assertEqual(len(obj_dynoverride_rule.override_values.bl_rna.properties), 2)
        self.assertEqual(len(obj_dynoverride_rule.original_values.bl_rna.properties), 2)

        obj_location = tuple(obj.location)
        self.assertEqual(tuple(obj_dynoverride_rule.override_values.location), obj_location)
        self.assertEqual(tuple(obj_dynoverride_rule.original_values.location), obj_location)

        obj_dynoverride_rule.override_values.location = (1, 2, 3)
        self.assertEqual(tuple(obj.location), obj_location)
        self.assertEqual(tuple(obj_dynoverride_rule.original_values.location), obj_location)

        bpy.context.view_layer.update()

        self.assertEqual(tuple(obj.location), obj_location)
        self.assertEqual(tuple(obj_dynoverride_rule.original_values.location), obj_location)

        obj_eval = bpy.context.view_layer.depsgraph.id_eval_get(obj)
        self.assertEqual(tuple(obj_dynoverride_rule.override_values.location), tuple(obj_eval.location))


TESTS = (
    TestDynamicOverrides,
)


def argparse_create():
    import argparse

    # When --help or no args are given, print this help
    description = "Test library overrides of blend file."
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument(
        "--output-dir",
        dest="output_dir",
        help="Where to output temp saved blendfiles",
        required=True,
    )
    parser.add_argument(
        "--test-dir",
        dest="test_dir",
        default=".",
        help="Where are the test blendfiles",
        required=False,
    )

    return parser


def main():
    args = argparse_create().parse_args()

    # Don't write thumbnails into the home directory.
    bpy.context.preferences.filepaths.file_preview_type = 'NONE'

    for Test in TESTS:
        Test(args).run_all_tests()


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + \
        (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    main()
