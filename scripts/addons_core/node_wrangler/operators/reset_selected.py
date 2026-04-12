# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Operator
from bpy.app.translations import pgettext_rpt as rpt_


from ..utils.nodes import (
    nw_check,
    nw_check_selected,
    transfer_links,
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
                and nw_check_selected(cls, context))

    @staticmethod
    def is_frame_node(node):
        return node.bl_idname == "NodeFrame"

    node_ignore = {"NodeFrame", "NodeReroute"}

    @staticmethod
    def get_zone_pair(tree, node):
        # Get paired output node.
        if hasattr(node, "paired_output"):
            return node, node.paired_output

        # Get paired input node.
        for input_node in tree.nodes:
            if hasattr(input_node, "paired_output"):
                if input_node.paired_output == node:
                    return input_node, node

        return None

    @staticmethod
    def swap_names(old_node, new_node):
        temp = old_node.name

        old_node.name = new_node.name
        new_node.name = temp

    @classmethod
    def ignore_node(cls, node):
        return node.bl_idname in cls.node_ignore

    def execute(self, context):
        node_active = context.active_node
        node_selected = context.selected_nodes
        node_tree = context.space_data.edit_tree
        valid_nodes = [n for n in node_selected if not self.ignore_node(n)]

        # Create output lists
        selected_node_names = [n.name for n in node_selected]

        # Check if valid nodes in selection
        if len(valid_nodes) < 1:
            self.report({'ERROR'}, "No valid node(s) in selection")
            return {'CANCELLED'}

        # Report nodes that are not valid
        if len(valid_nodes) != len(node_selected):
            valid_node_names = [n.name for n in valid_nodes]
            not_valid_names = list(set(selected_node_names) - set(valid_node_names))
            message = rpt_("Ignored {}").format(", ".join(not_valid_names))
            self.report({'INFO'}, message)

        props_to_copy = ("location", "height", "width", "select", "location_absolute", "parent")
        success_names = []
        nodes_to_delete = set()

        if node_active and node_active.select:
            active_node_name = node_active.name
        else:
            active_node_name = None

        # Run through all valid nodes
        for old_node in valid_nodes:
            if old_node in nodes_to_delete:
                continue

            zone_pair = self.get_zone_pair(node_tree, old_node)

            if zone_pair:
                # Zone nodes are always treated as a pair, even if only one is selected, reset both.
                old_input_node, old_output_node = zone_pair

                new_input_node = node_tree.nodes.new(old_input_node.bl_idname)
                new_output_node = node_tree.nodes.new(old_output_node.bl_idname)

                # Simulation input must be paired with the output.
                new_input_node.pair_with_output(new_output_node)

                for prop in props_to_copy:
                    setattr(new_input_node, prop, getattr(old_input_node, prop))
                    setattr(new_output_node, prop, getattr(old_output_node, prop))

                transfer_links(node_tree, old_input_node, new_input_node)
                transfer_links(node_tree, old_output_node, new_output_node)

                self.swap_names(old_input_node, new_input_node)
                self.swap_names(old_output_node, new_output_node)
                nodes_to_delete.add(old_input_node)
                nodes_to_delete.add(old_output_node)

                for node in zone_pair:
                    nodes_to_delete.add(node)
            else:
                new_node = node_tree.nodes.new(old_node.bl_idname)

                if hasattr(old_node, "node_tree"):
                    new_node.node_tree = old_node.node_tree
                    # Need to copy since it is usually False for assets, but usually True for regular groups
                    new_node.show_options = old_node.show_options

                transfer_links(node_tree, old_node, new_node)
                for prop in props_to_copy:
                    setattr(new_node, prop, getattr(old_node, prop))

                self.swap_names(old_node, new_node)
                nodes_to_delete.add(old_node)

        for node in nodes_to_delete:
            success_names.append(node.name)
            node_tree.nodes.remove(node)

        # Reselect all nodes
        if selected_node_names:
            for i in selected_node_names:
                node_tree.nodes[i].select = True

        if active_node_name is not None:
            node_tree.nodes.active = node_tree.nodes[active_node_name]

        message = rpt_("Successfully reset {}").format(", ".join(success_names))
        self.report({'INFO'}, message)
        return {'FINISHED'}
