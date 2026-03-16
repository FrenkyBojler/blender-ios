# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Operator
from bpy_extras.node_utils import connect_sockets
from bpy.app.translations import pgettext_rpt as rpt_

from itertools import chain

from ..utils.nodes import (
    nw_check,
    nw_check_active,
    nw_check_selected,
)


#### ------------------------------ OPERATORS ------------------------------ ####

class NODE_OT_reset_selected(Operator):
    """Revert nodes back to the default state, but keep connections"""
    bl_idname = "node.nw_reset_nodes"
    bl_label = "Reset Nodes"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return (nw_check(cls, context)
                and nw_check_selected(cls, context)
                and nw_check_active(cls, context))

    @staticmethod
    def is_frame_node(node):
        return node.bl_idname == "NodeFrame"

    group_node_types = {"CompositorNodeGroup", "GeometryNodeGroup", "ShaderNodeGroup"}
    # TODO All zone nodes are ignored here for now, because replacing one of the input/output pair breaks the zone.
    # It's possible to handle zones by using the `paired_output` function of an input node
    # and reconstruct the zone using the `pair_with_output` function.
    zone_node_types = {
        "GeometryNodeRepeatInput", "GeometryNodeRepeatOutput", "NodeClosureInput",
        "NodeClosureOutput", "GeometryNodeSimulationInput", "GeometryNodeSimulationOutput",
        "GeometryNodeForeachGeometryElementInput", "GeometryNodeForeachGeometryElementOutput",
    }
    node_ignore = group_node_types | zone_node_types | {"NodeFrame", "NodeReroute"}

    @classmethod
    def ignore_node(cls, node):
        return node.bl_idname in cls.node_ignore
    
    @staticmethod
    def transfer_links(tree, old_node, new_node):
        for inp in old_node.inputs:
            links = sorted(inp.links, key=lambda link: link.multi_input_sort_id)
            for link in links:
                is_muted = link.is_muted
                new_socket = new_node.inputs[inp.identifier]
                if new_socket.enabled and not new_socket.hide:
                    new_link = tree.links.new(link.from_socket, new_socket)
                    new_link.is_muted = is_muted

        for outp in old_node.outputs:
            for link in outp.links[:]:
                is_muted = link.is_muted
                new_socket = new_node.outputs[outp.identifier]
                if new_socket.enabled and not new_socket.hide:
                    is_multi_input = link.to_socket.is_multi_input
                    new_link = tree.links.new(new_socket, link.to_socket)
                    if is_multi_input:
                        new_link.swap_multi_input_sort_id(link)
                    new_link.is_muted = is_muted

    def execute(self, context):
        node_active = context.active_node
        node_selected = context.selected_nodes
        active_node_name = node_active.name if node_active.select else None
        valid_nodes = [n for n in node_selected if not self.ignore_node(n)]

        # Create output lists
        selected_node_names = [n.name for n in node_selected]
        success_names = []

        # Reset all valid children in a frame
        node_active_is_frame = False
        if len(node_selected) == 1 and self.is_frame_node(node_active):
            node_tree = node_active.id_data
            children = [n for n in node_tree.nodes if n.parent == node_active]
            if children:
                valid_nodes = [n for n in children if not self.ignore_node(n)]
                selected_node_names = [n.name for n in children if not self.ignore_node(n)]
                node_active_is_frame = True

        # Check if valid nodes in selection
        if not (len(valid_nodes) > 0):
            # Check for frames only
            frames_selected = [n for n in node_selected if self.is_frame_node(n)]
            if (len(frames_selected) > 1 and len(frames_selected) == len(node_selected)):
                self.report({'ERROR'}, "Please select only 1 frame to reset")
            else:
                self.report({'ERROR'}, "No valid node(s) in selection")
            return {'CANCELLED'}

        # Report nodes that are not valid
        if len(valid_nodes) != len(node_selected) and node_active_is_frame is False:
            valid_node_names = [n.name for n in valid_nodes]
            not_valid_names = list(set(selected_node_names) - set(valid_node_names))
            message = rpt_("Ignored {}").format(", ".join(not_valid_names))
            self.report({'INFO'}, message)

        props_to_copy = ("location", "height", "width", "select", "location_absolute", "parent")

        # Run through all valid nodes
        for node in valid_nodes:
            node_tree = node.id_data

            props = {j: getattr(node, j) for j in props_to_copy}

            new_node = node_tree.nodes.new(node.bl_idname)
            self.transfer_links(node_tree, node, new_node)

            for prop in props_to_copy:
                setattr(new_node, prop, props[prop])

            node_name = node.name
            node_tree.nodes.remove(node)
            new_node.name = node_name

            success_names.append(new_node.name)

        if active_node_name is not None:
            node_tree.nodes[active_node_name].select = True
            node_tree.nodes.active = node_tree.nodes[active_node_name]

        message = rpt_("Successfully reset {}").format(", ".join(success_names))
        self.report({'INFO'}, message)
        return {'FINISHED'}
