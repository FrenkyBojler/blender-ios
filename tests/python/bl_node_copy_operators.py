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


class AbstractNodeCopyOperatorTest(unittest.TestCase):
    testfile = "node_copy_operators.blend"

    def open_file(self):
        bpy.ops.wm.open_mainfile(filepath=str(self.testdir / self.testfile))
        self.assertEqual(bpy.data.version, (5, 1, 14))

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

    def compare_value(self, bl_idname, value_a, value_b):
        vector_value_types = {
            "NodeSocketMatrix", "NodeSocketRotation", "NodeSocketVector", "NodeSocketVectorFactor",
            "NodeSocketVectorPercentage", "NodeSocketVectorTranslation", "NodeSocketVectorDirection",
            "NodeSocketVectorVelocity", "NodeSocketVectorAcceleration", "NodeSocketVectorEuler",
            "NodeSocketVectorXYZ", "NodeSocketColor"
        }
        if bl_idname in vector_value_types:
            for comp_a, comp_b in zip(value_a, value_b):
                self.assertEqual(comp_a, comp_b)
        else:
            self.assertEqual(value_a, value_b)

    # Validate node socket properties and connections in the tree.
    # Links to/from the socket are compared to expected values using the node and socket maps.
    def compare_socket(self, test_socket, node_map, socket_map):
        expected_socket = socket_map[test_socket]
        with self.subTest(test_socket=test_socket.name, expected_socket=expected_socket.name):
            # Generic socket properties
            self.assertEqual(test_socket.name, expected_socket.name)
            self.assertEqual(test_socket.bl_idname, expected_socket.bl_idname)
            self.assertEqual(test_socket.type, expected_socket.type)
            self.assertEqual(test_socket.description, expected_socket.description)
            self.assertEqual(test_socket.is_output, expected_socket.is_output)

            # Input value
            if not expected_socket.is_output:
                self.assertEqual(test_socket.hide_value, expected_socket.hide_value)
                test_has_value = hasattr(test_socket, "default_value")
                expected_has_value = hasattr(expected_socket, "default_value")
                self.assertEqual(test_has_value, expected_has_value)
                if test_has_value and expected_has_value:
                    self.compare_value(expected_socket.bl_idname, test_socket.default_value, expected_socket.default_value)

            # Links
            self.assertEqual(test_socket.is_linked, expected_socket.is_linked)
            if expected_socket.is_linked:
                self.assertEqual(len(test_socket.links), len(expected_socket.links))
                for test_link, expected_link in zip(test_socket.links, expected_socket.links):
                    if expected_socket.is_output:
                        self.assertEqual(node_map[test_link.to_node], expected_link.to_node)
                        self.assertEqual(socket_map[test_link.to_socket], expected_link.to_socket)
                    else:
                        self.assertEqual(node_map[test_link.from_node], expected_link.from_node)
                        self.assertEqual(socket_map[test_link.from_socket], expected_link.from_socket)

    # Validate a node against the expected data using the node map.
    def compare_nodes(self, test_node, node_map, socket_map):
        expected_node = node_map[test_node]

        self.assertEqual(len(test_node.inputs), len(expected_node.inputs))
        self.assertEqual(len(test_node.outputs), len(expected_node.outputs))
        for test_socket in test_node.inputs:
            self.compare_socket(test_socket, node_map, socket_map)
        for test_socket in test_node.outputs:
            self.compare_socket(test_socket, node_map, socket_map)

    # Validate the tree interface settings of a node group.
    def compare_tree_interface(self, test_tree, expected_tree):
        test_items = test_tree.interface.items_tree
        expected_items = expected_tree.interface.items_tree
        self.assertEqual(len(test_items), len(expected_items))
        for test_item, expected_item in zip(test_items, expected_items):
            self.assertEqual(test_item.index, expected_item.index)
            self.assertEqual(test_item.item_type, expected_item.item_type)
            # Find expected parent panel by index from the expected items list.
            # Item with index -1 is the root panel and can be ignored.
            if test_item.parent.index >= 0:
                expected_parent = expected_items[test_item.parent.index]
                self.assertEqual(expected_parent, expected_item.parent)
            else:
                self.assertEqual(test_item.parent.index, -1)
            self.assertEqual(test_item.position, expected_item.position)

            if expected_item.item_type == 'SOCKET':
                # General properties.
                self.assertEqual(test_item.bl_socket_idname, expected_item.bl_socket_idname)
                self.assertEqual(test_item.in_out, expected_item.in_out)
                self.assertEqual(test_item.name, expected_item.name)
                self.assertEqual(test_item.description, expected_item.description)
                self.assertEqual(test_item.optional_label, expected_item.optional_label)
                self.assertEqual(test_item.socket_type, expected_item.socket_type)
                self.assertEqual(test_item.structure_type, expected_item.structure_type)
                self.assertEqual(test_item.is_panel_toggle, expected_item.is_panel_toggle)
                self.assertEqual(test_item.layer_selection_field, expected_item.layer_selection_field)

                # Default value.
                self.assertEqual(test_item.hide_value, expected_item.hide_value)
                self.assertEqual(test_item.hide_in_modifier, expected_item.hide_in_modifier)
                self.assertEqual(test_item.default_input, expected_item.default_input)
                self.assertEqual(test_item.menu_expanded, expected_item.menu_expanded)
                if hasattr(expected_item, "default_value"):
                    self.compare_value(expected_item.bl_socket_idname, test_item.default_value, expected_item.default_value)
                if hasattr(expected_item, "min_value"):
                    self.assertEqual(test_item.min_value, expected_item.min_value)
                if hasattr(expected_item, "max_value"):
                    self.assertEqual(test_item.max_value, expected_item.max_value)
                if hasattr(expected_item, "subtype"):
                    self.assertEqual(test_item.subtype, expected_item.subtype)
                if hasattr(expected_item, "dimensions"):
                    self.assertEqual(test_item.dimensions, expected_item.dimensions)

                # Attribute settings.
                self.assertEqual(test_item.attribute_domain, expected_item.attribute_domain)
                self.assertEqual(test_item.default_attribute_name, expected_item.default_attribute_name)

            if expected_item.item_type == 'PANEL':
                self.assertEqual(test_item.name, expected_item.name)
                self.assertEqual(test_item.persistent_uid, expected_item.persistent_uid)
                self.assertEqual(test_item.description, expected_item.description)
                self.assertEqual(test_item.default_closed, expected_item.default_closed)

    # Add all sockets of mapped nodes to their own dictionary, assuming the socket order is the same.
    @staticmethod
    def build_socket_map(node_map):
        socket_map = dict()
        for test_node, expected_node in node_map.items():
            for test_socket, expected_socket in zip(test_node.inputs, expected_node.inputs):
                socket_map[test_socket] = expected_socket
            for test_socket, expected_socket in zip(test_node.outputs, expected_node.outputs):
                socket_map[test_socket] = expected_socket
        return socket_map


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

    # Relying on context.selected_nodes and context.active_node does not work for many/most node operators
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

    def test_make_node_group_single(self):
        test_nodes = ["TestNode.Defaults", "TestNode.InputValues", "TestNode.Links"]
        expected_group_nodes = ["GroupNode.Defaults", "GroupNode.InputValues", "GroupNode.Links"]
        # Reroute nodes with links to the test node.
        test_links = [
            "InputLink.Geometry",
            "InputLink.Float",
            "InputLink.Int",
            "InputLink.Bool",
            "InputLink.Vector",
            "InputLink.Color",
            "InputLink.Matrix",
            "InputLink.String",
            "InputLink.MenuUndefined",
            "InputLink.MenuDefined",
            "InputLink.MenuConflict",
            "InputLink.Object", 
            "InputLink.Collection",
            "InputLink.Image",
            "InputLink.Material",
            "InputLink.OptLabel",
            "InputLink.HideValue",
            "InputLink.HideInModifier",
            "InputLink.LayerSelect",
            "InputLink.Expanded",
            "InputLink.Dynamic",
            "InputLink.Field",
            "InputLink.Grid",
            "InputLink.Single",
            "InputLink.Dim2",
            "InputLink.DefaultNormal",
            "InputLink.Panel",
            "InputLink.Panel Socket",
            "InputLink.PanelClosed Socket",

            "OutputLink.Geometry",
            "OutputLink.Float",
            "OutputLink.Int",
            "OutputLink.Bool",
            "OutputLink.Vector",
            "OutputLink.Color",
            "OutputLink.Matrix",
            "OutputLink.String",
            "OutputLink.Menu",
            "OutputLink.Object",
            "OutputLink.Collection",
            "OutputLink.Image",
            "OutputLink.Material",
            "OutputLink.Panel",
            "OutputLink.Panel Socket",
            "OutputLink.PanelClosed Socket",
        ]
        # Reroute nodes with links to the expected node.
        expected_links = [
            "InputLink.Geometry.001",
            "InputLink.Float.001",
            "InputLink.Int.001",
            "InputLink.Bool.001",
            "InputLink.Vector.001",
            "InputLink.Color.001",
            "InputLink.Matrix.001",
            "InputLink.String.001",
            "InputLink.MenuUndefined.001",
            "InputLink.MenuDefined.001",
            "InputLink.MenuConflict.001",
            "InputLink.Object.001",
            "InputLink.Collection.001",
            "InputLink.Image.001",
            "InputLink.Material.001",
            "InputLink.OptLabel.001",
            "InputLink.HideValue.001",
            "InputLink.HideInModifier.001",
            "InputLink.LayerSelect.001",
            "InputLink.Expanded.001",
            "InputLink.Dynamic.001",
            "InputLink.Field.001",
            "InputLink.Grid.001",
            "InputLink.Single.001",
            "InputLink.Dim2.001",
            "InputLink.DefaultNormal.001",
            "InputLink.Panel.001",
            "InputLink.Panel Socket.001",
            "InputLink.PanelClosed Socket.001",

            "OutputLink.Geometry.001",
            "OutputLink.Float.001",
            "OutputLink.Int.001",
            "OutputLink.Bool.001",
            "OutputLink.Vector.001",
            "OutputLink.Color.001",
            "OutputLink.Matrix.001",
            "OutputLink.String.001",
            "OutputLink.Menu.001",
            "OutputLink.Object.001",
            "OutputLink.Collection.001",
            "OutputLink.Image.001",
            "OutputLink.Material.001",
            "OutputLink.Panel.001",
            "OutputLink.Panel Socket.001",
            "OutputLink.PanelClosed Socket.001",
        ]
        for test_node_name, expected_group_node_name in zip(test_nodes, expected_group_nodes):
            with self.subTest(test_node=test_node_name, expected_group_node=expected_group_node_name):
                self.open_file()
                tree = bpy.data.node_groups['Geometry Nodes']
                test_node = tree.nodes[test_node_name]
                expected_group_node = tree.nodes[expected_group_node_name]

                with node_editor_context_override(selected_nodes=[test_node]):
                    bpy.ops.node.group_make()
                group_node = tree.nodes.active

                # Map operator result to expected nodes.
                node_map = dict()
                node_map[group_node] = expected_group_node
                # Linked reroute nodes are gathered in frame nodes for convenience, map frame children in the same order.
                for test_link_name, expected_link_name in zip(test_links, expected_links):
                    node_map[tree.nodes[test_link_name]] = tree.nodes[expected_link_name]
                socket_map = self.build_socket_map(node_map)

                # Compare generated group node to expected node.
                self.compare_nodes(group_node, node_map, socket_map)
                # Compare generated group tree interface to expected tree.
                self.compare_tree_interface(group_node.node_tree, expected_group_node.node_tree)


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
