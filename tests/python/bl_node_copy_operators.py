# SPDX-FileCopyrightText: 2021-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import sys
import unittest
import tempfile
import math
from dataclasses import dataclass

import bpy

args = None


base_idname = {
    "VALUE": "NodeSocketFloat",
    "INT": "NodeSocketInt",
    "BOOLEAN": "NodeSocketBool",
    "ROTATION": "NodeSocketRotation",
    "VECTOR": "NodeSocketVector",
    "RGBA": "NodeSocketColor",
    "STRING": "NodeSocketString",
    "SHADER": "NodeSocketShader",
    "OBJECT": "NodeSocketObject",
    "IMAGE": "NodeSocketImage",
    "GEOMETRY": "NodeSocketGeometry",
    "COLLECTION": "NodeSocketCollection",
    "TEXTURE": "NodeSocketTexture",
    "MATERIAL": "NodeSocketMaterial",
}


subtype_idname = {
    ("VALUE", "NONE"): "NodeSocketFloat",
    ("VALUE", "UNSIGNED"): "NodeSocketFloatUnsigned",
    ("VALUE", "PERCENTAGE"): "NodeSocketFloatPercentage",
    ("VALUE", "FACTOR"): "NodeSocketFloatFactor",
    ("VALUE", "ANGLE"): "NodeSocketFloatAngle",
    ("VALUE", "TIME"): "NodeSocketFloatTime",
    ("VALUE", "TIME_ABSOLUTE"): "NodeSocketFloatTimeAbsolute",
    ("VALUE", "DISTANCE"): "NodeSocketFloatDistance",
    ("INT", "NONE"): "NodeSocketInt",
    ("INT", "UNSIGNED"): "NodeSocketIntUnsigned",
    ("INT", "PERCENTAGE"): "NodeSocketIntPercentage",
    ("INT", "FACTOR"): "NodeSocketIntFactor",
    ("BOOLEAN", "NONE"): "NodeSocketBool",
    ("ROTATION", "NONE"): "NodeSocketRotation",
    ("VECTOR", "NONE"): "NodeSocketVector",
    ("VECTOR", "FACTOR"): "NodeSocketVectorFactor",
    ("VECTOR", "PERCENTAGE"): "NodeSocketVectorPercentage",
    ("VECTOR", "TRANSLATION"): "NodeSocketVectorTranslation",
    ("VECTOR", "DIRECTION"): "NodeSocketVectorDirection",
    ("VECTOR", "VELOCITY"): "NodeSocketVectorVelocity",
    ("VECTOR", "ACCELERATION"): "NodeSocketVectorAcceleration",
    ("VECTOR", "EULER"): "NodeSocketVectorEuler",
    ("VECTOR", "XYZ"): "NodeSocketVectorXYZ",
    ("RGBA", "NONE"): "NodeSocketColor",
    ("STRING", "NONE"): "NodeSocketString",
    ("STRING", "FILEPATH"): "NodeSocketStringFilePath",
    ("SHADER", "NONE"): "NodeSocketShader",
    ("OBJECT", "NONE"): "NodeSocketObject",
    ("IMAGE", "NONE"): "NodeSocketImage",
    ("GEOMETRY", "NONE"): "NodeSocketGeometry",
    ("COLLECTION", "NONE"): "NodeSocketCollection",
    ("TEXTURE", "NONE"): "NodeSocketTexture",
    ("MATERIAL", "NONE"): "NodeSocketMaterial",
}


@dataclass
class SocketSpec():
    name: str
    identifier: str
    type: str
    subtype: str = 'NONE'
    hide_value: bool = False
    hide_in_modifier: bool = False
    default_value: object = None
    min_value: object = None
    max_value: object = None
    internal_links: int = 1
    external_links: int = 1

    @property
    def base_idname(self):
        return base_idname[self.type]

    @property
    def subtype_idname(self):
        return subtype_idname[(self.type, self.subtype)]


class AbstractNodeCopyOperatorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.testdir = args.testdir
        cls._tempdir = tempfile.TemporaryDirectory()
        cls.tempdir = pathlib.Path(cls._tempdir.name)

    def setUp(self):
        self.assertTrue(self.testdir.exists(),
                        'Test dir {0} should exist'.format(self.testdir))

    def tearDown(self):
        self._tempdir.cleanup()


# Provide a valid context override to run node editor operators
def node_editor_context_override(context=None, selected_nodes=[], active_node=None):
    if context is None:
        context = bpy.context
    if active_node is None:
        active_node = selected_nodes[0] if selected_nodes else None
    area = next(area for area in context.screen.areas if area.type == 'NODE_EDITOR')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces[0]
    tree = space.edit_tree

    context_override = context.copy()
    context_override["area"] = area
    context_override["region"] = region
    context_override["space_data"] = space
    context_override["selected_nodes"] = selected_nodes
    context_override["active_node"] = active_node

    # XXX Relying on context.selected_nodes and context.active_node does not work for many/most node operators
    # because they rely on actual selected/active nodes in the tree, rather than the context.
    for node in tree.nodes:
        node.select = False
    for node in selected_nodes:
        node.select = True
    tree.nodes.active = active_node

    return context.temp_override(**context_override)


class NodeMakeGroupTest(AbstractNodeCopyOperatorTest):
    test_nodes = ["TestNode.Defaults", "TestNode.InputValues", "TestNode.Links"]
    group_nodes_single = ["GroupNode.Defaults", "GroupNode.InputValues", "GroupNode.Links"]
    group_node_all = "GroupNode.All"

    def open_file(self):
        bpy.ops.wm.open_mainfile(filepath=str(self.testdir / "node_copy_operators.blend"))
        self.assertEqual(bpy.data.version, (5, 1, 12))

    def compare_nodes(self, test_node, expected_node):
        self.assertEqual(len(test_node.inputs), len(expected_node.inputs))
        self.assertEqual(len(test_node.outputs), len(expected_node.outputs))

    def compare_tree_interface(self, test_tree, expected_tree):
        test_items = test_tree.interface.items_tree
        expected_items = expected_tree.interface.items_tree
        self.assertEqual(len(test_items), len(expected_items))
        for te, ex in zip(test_items, expected_items):
            te_io = getattr(te, "in_out", None)
            ex_io = getattr(ex, "in_out", None)
            # print(f"{te.item_type}|{ex.item_type}, {te_io}|{ex_io}, {te.name}|{ex.name}")

    def make_node_group_single(self, test_node, expected_node):
        tree = test_node.id_data

        with node_editor_context_override(selected_nodes=[test_node]):
            bpy.ops.node.group_make()
        group_node = tree.nodes.active

        # Compare generated group node to expected node.
        self.compare_nodes(group_node, expected_node)
        # Compare generated group tree interface to expected tree.
        self.compare_tree_interface(group_node.node_tree, expected_node.node_tree)

    def test_make_node_group_single(self):
        self.open_file()
        tree = bpy.data.node_groups['Geometry Nodes']
        self.make_node_group_single(tree.nodes["TestNode.Defaults"], tree.nodes["GroupNode.Defaults"])
        self.make_node_group_single(tree.nodes["TestNode.InputValues"], tree.nodes["GroupNode.InputValues"])
        self.make_node_group_single(tree.nodes["TestNode.Links"], tree.nodes["GroupNode.Links"])


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
