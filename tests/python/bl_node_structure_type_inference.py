# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import sys
import tempfile
import bpy
import unittest

args = None


class StructureTypeInferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.testdir = args.testdir

    def setUp(self):
        self.assertTrue(self.testdir.exists(),
                        "Test dir {0} should exist".format(self.testdir))

    def load_testfile(self):
        bpy.ops.wm.open_mainfile(filepath=str(self.testdir / "structure_type_inference.blend"))

    def assertDynamic(self, socket):
        self.assertEqual(socket.inferred_structure_type, "DYNAMIC")

    def assertSingle(self, socket):
        self.assertEqual(socket.inferred_structure_type, "SINGLE")

    def assertField(self, socket):
        self.assertEqual(socket.inferred_structure_type, "FIELD")

    def test_empty_group(self):
        self.load_testfile()
        tree = bpy.data.node_groups["Empty Group"]

        node = tree.nodes["Group Input"]
        self.assertDynamic(node.outputs["Geometry"])
        self.assertDynamic(node.outputs["Value"])

        node = tree.nodes["Group Output"]
        self.assertSingle(node.inputs["Geometry"])
        self.assertSingle(node.inputs["Value"])

    def test_math_node(self):
        self.load_testfile()
        tree = bpy.data.node_groups["Math Node"]

        node = tree.nodes["Group Input"]
        self.assertDynamic(node.outputs["A"])
        self.assertDynamic(node.outputs["B"])

        node = tree.nodes["Group Output"]
        self.assertDynamic(node.inputs["Out"])

    def test_cube_node(self):
        self.load_testfile()
        tree = bpy.data.node_groups["Cube"]

        node = tree.nodes["Group Input"]
        self.assertSingle(node.outputs["Size"])
        self.assertSingle(node.outputs["Vertices X"])
        self.assertSingle(node.outputs["Vertices Y"])
        self.assertSingle(node.outputs["Vertices Z"])

        node = tree.nodes["Group Output"]
        self.assertSingle(node.inputs["Mesh"])
        self.assertField(node.inputs["UV Map"])


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
