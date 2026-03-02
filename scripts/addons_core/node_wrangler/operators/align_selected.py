# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Operator
from bpy.props import IntProperty

from copy import copy

from ..utils.nodes import (
    NWBase,
    nw_check,
    nw_check_not_empty,
    nw_check_selected,
    get_nodes_links,
)


#### ------------------------------ OPERATORS ------------------------------ ####

class NODE_OT_align_selected(Operator, NWBase):
    '''Align selected nodes in a grid pattern'''
    bl_idname = "node.nw_align_nodes"
    bl_label = "Align Nodes"
    bl_options = {'REGISTER', 'UNDO'}

    margin: IntProperty(
        name='Margin',
        description='The amount of space between nodes',
        default=50,
    )

    @classmethod
    def poll(cls, context):
        return nw_check(cls, context) and nw_check_not_empty(cls, context) and nw_check_selected(cls, context)
    
    @staticmethod
    def parent_depth(node):        
        for i in range(100_000_000):
            if node is None:
                return i
            
            node = node.parent
            
        raise Exception("This should be unreachable.")
    
    def arrange_nodes(self, nodes):
        margin = self.margin

        # Check if nodes should be laid out horizontally or vertically
        # use dimension to get center of node, not corner
        x_locs = [n.location_absolute.x + (n.dimensions.x / 2) for n in nodes]
        y_locs = [n.location_absolute.y - (n.dimensions.y / 2) for n in nodes]
        x_range = max(x_locs) - min(x_locs)
        y_range = max(y_locs) - min(y_locs)
        mid_x = (max(x_locs) + min(x_locs)) / 2
        mid_y = (max(y_locs) + min(y_locs)) / 2
        horizontal = x_range > y_range

        # Sort selection by location of node mid-point
        if horizontal:
            nodes = sorted(nodes, key=lambda n: n.location_absolute.x + (n.dimensions.x / 2))
        else:
            nodes = sorted(nodes, key=lambda n: n.location_absolute.y - (n.dimensions.y / 2), reverse=True)

        # Alignment
        current_pos = 0
        for node in nodes:
            if node.bl_idname == "NodeFrame":
                continue

            current_margin = margin

            # Use a smaller margin for hidden nodes.
            current_margin = current_margin * 0.5 if node.hide else current_margin

            if horizontal:
                node.location_absolute.x = current_pos
                current_pos += current_margin + node.dimensions.x
                node.location_absolute.y = mid_y + (node.dimensions.y / 2)
            else:
                # `node.bl_height_min` is the min size of a collapsed node, +6 for the outlines and margins.
                hide_offset = (node.dimensions.y - (node.bl_height_min + 6)) / 2 if node.hide else 0

                # Hidden nodes center their sockets around the label instead of below.
                node.location_absolute.y = current_pos - hide_offset

                # Use half-margin for vertical alignment.
                current_pos -= (current_margin * 0.3) + node.dimensions.y

                node.location_absolute.x = mid_x - (node.dimensions.x / 2)

    def execute(self, context):
        nodes = context.selected_nodes
        parent_map = {}

        for node in nodes:
            children = parent_map.get(node.parent, None)
            if children is None:
                parent_map[node.parent] = []

            parent_map[node.parent].append(node)

        if not nodes:
            self.report({'WARNING'}, "No nodes to arrange in selection.")
            return {'CANCELLED'}

        active_loc = None
        if context.active_node:
            active_loc = copy(context.active_node.location_absolute)  # make a copy, not a reference

        # Check if nodes should be laid out horizontally or vertically
        # use dimension to get center of node, not corner
        x_locs = [n.location_absolute.x + (n.dimensions.x / 2) for n in nodes]
        y_locs = [n.location_absolute.y - (n.dimensions.y / 2) for n in nodes]
        x_range = max(x_locs) - min(x_locs)
        y_range = max(y_locs) - min(y_locs)
        mid_x = (max(x_locs) + min(x_locs)) / 2
        mid_y = (max(y_locs) + min(y_locs)) / 2
        horizontal = x_range > y_range

        sorted_keys = sorted(parent_map.keys(), key=self.parent_depth, reverse=True)
        for parent in sorted_keys:
            children = parent_map[parent]
            for child in children:
                child.label = str(self.parent_depth(parent) + 1)

            self.arrange_nodes(children)

        # If active node is selected, center nodes around it
        if active_loc is not None:
            active_loc_diff = active_loc - context.active_node.location_absolute
            for node in nodes:
                node.location_absolute += active_loc_diff
        else:  # Position nodes centered around where they used to be
            locs = ([n.location_absolute.x + (n.dimensions.x / 2) for n in nodes]
                    ) if horizontal else ([n.location_absolute.y - (n.dimensions.y / 2) for n in nodes])
            new_mid = (max(locs) + min(locs)) / 2
            for node in nodes:
                if horizontal:
                    node.location_absolute.x += (mid_x - new_mid)
                else:
                    node.location_absolute.y += (mid_y - new_mid)

        return {'FINISHED'}
