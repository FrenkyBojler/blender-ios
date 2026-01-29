# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import unittest
import bpy
import pathlib
import sys
import tempfile
from itertools import chain

args = None
testfile = "link_drag_search.blend"


def open_test_file():
    bpy.ops.wm.open_mainfile(filepath=str(args.testdir / testfile))


def save_test_file():
    bpy.ops.wm.save_mainfile(filepath=str(args.testdir / testfile))


def selected_nodes(tree):
    return [node for node in tree.nodes if node.select]


def select_nodes(tree, selected_nodes, active_node=None):
    for node in tree.nodes:
        node.select = False
    for node in selected_nodes:
        node.select = True
    tree.nodes.active = active_node if active_node else (selected_nodes[0] if selected_nodes else None)


# Provide a valid context override to run node editor operators
def node_editor_context_override(context, tree, selected_nodes=[], active_node=None, data_pointers=None):
    window = context.window if context.window else next(
        window for window in context.window_manager.windows if window.screen is not None)
    screen = context.screen if context.screen else window.screen
    area = next(area for area in screen.areas if area.type == 'NODE_EDITOR' and area.spaces[0].tree_type == tree.bl_idname)
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces[0]

    # Explicitly set the space tree, otherwise requires a context update to ensure
    # that the space tree matches the active modifier tree.
    space.node_tree = tree
    # Relying on context.selected_nodes and context.active_node does not work for many/most node operators
    # because they rely on actual selected/active nodes in the tree, rather than the context.
    select_nodes(tree, selected_nodes, active_node)

    context_override = context.copy()
    context_override["window"] = window
    context_override["screen"] = screen
    context_override["area"] = area
    context_override["region"] = region
    context_override["space_data"] = space
    context_override["selected_nodes"] = selected_nodes
    context_override["active_node"] = tree.nodes.active

    if data_pointers:
        for key, value in data_pointers.items():
            context_override[key] = value

    return context.temp_override(**context_override)


_socket_idnames = [
    "NodeSocketBool",
    "NodeSocketBundle",
    "NodeSocketClosure",
    "NodeSocketCollection",
    "NodeSocketColor",
    "NodeSocketFloat",
    "NodeSocketFont",
    "NodeSocketGeometry",
    "NodeSocketImage",
    "NodeSocketInt",
    "NodeSocketMask",
    "NodeSocketMaterial",
    "NodeSocketMatrix",
    "NodeSocketMenu",
    "NodeSocketObject",
    "NodeSocketRotation",
    "NodeSocketScene",
    "NodeSocketShader",
    "NodeSocketSound",
    "NodeSocketString",
    "NodeSocketText",
    "NodeSocketTexture",
    "NodeSocketVector",
    "NodeSocketVector2D",
    "NodeSocketVector4D",
    "NodeSocketVirtual",
]


class AbstractNodeCopyOperatorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tempdir = tempfile.TemporaryDirectory()
        cls.tempdir = pathlib.Path(cls._tempdir.name)

    def setUp(self):
        self.assertTrue(args.testdir.exists(),
                        'Test dir {0} should exist'.format(args.testdir))
        open_test_file()

    def tearDown(self):
        self._tempdir.cleanup()

    def run_tree_type_tests(self, tree, group_node_type):
        # Create node group with a socket of every type.
        group_tree = bpy.data.node_groups.new(name=f"{tree.bl_idname} Sockets", type=tree.bl_idname)
        for socket_idname in _socket_idnames:
            # Add a socket for all supported types.
            try:
                group_tree.interface.new_socket(name=socket_idname, socket_type=socket_idname)
            except TypeError:
                print(f"Skipping unsupported socket type {socket_idname} in tree {tree.bl_idname}")

        group_node = tree.nodes.new(type=group_node_type)
        group_node.name = "Sockets"
        group_node.node_tree = group_tree

        for socket in chain(group_node.inputs, group_node.outputs):
            with node_editor_context_override(bpy.context, tree, data_pointers = {"socket": socket}):
                bpy.ops.node.link_drag_operation_test(count_link_operations=True)
            link_ops_count = tree["link_operations_count"]
            print(f"{tree.bl_idname}: {socket.bl_idname} has {link_ops_count} link operations")
            for link_op_index in range(link_ops_count):
                with node_editor_context_override(bpy.context, tree, data_pointers = {"socket": socket}):
                    bpy.ops.node.link_drag_operation_test(link_operation_index=link_op_index)

                added_nodes = selected_nodes(tree)
                for node in added_nodes:
                    tree.nodes.remove(node)


    def test_compositor_nodes(self):
        self.run_tree_type_tests(bpy.data.node_groups["Compositor Nodes"], "CompositorNodeGroup")

    def test_geometry_nodes(self):
        self.run_tree_type_tests(bpy.data.node_groups["Geometry Nodes"], "GeometryNodeGroup")

    def test_shader_nodes(self):
        self.run_tree_type_tests(bpy.data.materials["Material"].node_tree, "ShaderNodeGroup")


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
