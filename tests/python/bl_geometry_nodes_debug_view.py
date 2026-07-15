# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
import unittest

import bpy


class GeometryNodesDebugViewTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def create_node_group(self):
        group = bpy.data.node_groups.new("Geometry Nodes", "GeometryNodeTree")
        group.interface.new_socket(
            name="Geometry", in_out='OUTPUT', socket_type="NodeSocketGeometry")

        output = group.nodes.new("NodeGroupOutput")
        normal_geometry = group.nodes.new("GeometryNodeMeshCube")
        debug_geometry = group.nodes.new("GeometryNodeMeshIcoSphere")
        warning = group.nodes.new("GeometryNodeWarning")
        warning.inputs["Show"].default_value = True
        warning.inputs["Message"].default_value = "Debug View evaluated"

        switch = group.nodes.new("GeometryNodeSwitch")
        switch.input_type = 'GEOMETRY'
        group.links.new(warning.outputs["Show"], switch.inputs["Switch"])
        group.links.new(debug_geometry.outputs["Mesh"], switch.inputs["True"])

        viewer = group.nodes.new("GeometryNodeViewer")
        viewer.viewer_items.new('GEOMETRY', "Geometry")
        group.links.new(switch.outputs["Output"], viewer.inputs["Geometry"])
        group.links.new(normal_geometry.outputs["Mesh"], output.inputs["Geometry"])
        return group, viewer

    def create_object_with_modifier(self, name, group):
        mesh = bpy.data.meshes.new(name + " Mesh")
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.collection.objects.link(obj)
        modifier = obj.modifiers.new("Geometry Nodes", 'NODES')
        modifier.node_group = group
        return obj, modifier

    def evaluate(self, obj):
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated_obj = obj.evaluated_get(depsgraph)
        return len(evaluated_obj.data.vertices)

    def warning_messages(self, modifier):
        return [warning.message for warning in modifier.node_warnings]

    def test_debug_view_evaluation_and_modifier_isolation(self):
        group, viewer = self.create_node_group()
        obj_a, modifier_a = self.create_object_with_modifier("Object A", group)
        obj_b, modifier_b = self.create_object_with_modifier("Object B", group)

        self.assertIsNone(viewer.inputs.get("Show"))
        modifier_a.show_debug_views = True
        modifier_b.show_debug_views = False

        self.assertEqual(self.evaluate(obj_a), 8)
        self.assertEqual(self.evaluate(obj_b), 8)
        self.assertEqual(self.warning_messages(modifier_a), [])
        self.assertEqual(self.warning_messages(modifier_b), [])

        viewer.is_debug_view = True
        show_socket = viewer.inputs["Show"]
        show_socket.default_value = False

        self.assertEqual(self.evaluate(obj_a), 8)
        self.assertEqual(self.warning_messages(modifier_a), [])

        show_socket.default_value = True

        self.assertEqual(self.evaluate(obj_a), 8)
        self.assertEqual(self.evaluate(obj_b), 8)
        self.assertIn("Debug View evaluated", self.warning_messages(modifier_a))
        self.assertEqual(self.warning_messages(modifier_b), [])

    def test_show_socket_preserves_link(self):
        group, viewer = self.create_node_group()
        viewer.is_debug_view = True
        show_socket = viewer.inputs["Show"]
        boolean = group.nodes.new("FunctionNodeInputBool")
        group.links.new(boolean.outputs["Boolean"], show_socket)

        viewer.is_debug_view = False
        self.assertTrue(show_socket.is_unavailable)
        self.assertEqual(len(show_socket.links), 1)

        viewer.is_debug_view = True
        self.assertFalse(show_socket.is_unavailable)
        self.assertEqual(len(show_socket.links), 1)
        self.assertEqual(show_socket.links[0].from_node, boolean)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
