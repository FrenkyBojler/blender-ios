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


weird_offset = 10
frame_margin = 30

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
    
    def frame_children(self, frame):
        for node in self.tree.nodes:
            if node.parent == frame:
                yield node
    
    def get_width(self, node):
        if node.bl_static_type == 'FRAME':
            return self.get_right(node) - self.get_left(node)
        else:
            return node.width
    
    def get_height(self, node):
        if node.bl_static_type == 'FRAME':
            return self.get_top(node) - self.get_bottom(node)
        else:
            return node.width * node.dimensions.y/node.dimensions.x

    def get_left(self, node):
        if node.bl_static_type == 'REROUTE':
            return node.location_absolute.x
        elif node.bl_static_type == 'FRAME':
            return min(self.get_left(node) for node in self.frame_children(node)) - frame_margin
        else:
            return node.location_absolute.x

    def get_center(self, node):
        if node.bl_static_type == 'REROUTE':
            return node.location_absolute.x
        else:
            return node.location_absolute.x + (0.5 * node.width)

    def get_right(self, node):
        if node.bl_static_type == 'REROUTE':
            return node.location_absolute.x
        elif node.bl_static_type == 'FRAME':
            return max(self.get_right(node) for node in self.frame_children(node)) + frame_margin
        else:
            return node.location_absolute.x + node.width

    def get_top(self, node):
        if node.bl_static_type == 'REROUTE':
            return node.location_absolute.y
        elif node.bl_static_type == 'FRAME':
            return max(self.get_top(node) for node in self.frame_children(node)) + frame_margin
        elif node.hide:
            return node.location_absolute.y + (0.5 * self.get_height(node)) - weird_offset
        else:
            return node.location_absolute.y

    def get_middle(self, node):
        if node.bl_static_type == 'REROUTE':
            return node.location_absolute.y
        elif node.hide:
            return node.location_absolute.y - weird_offset
        else:
            return node.location_absolute.y - (0.5 * self.get_height(node))

    def get_bottom(self, node):
        if node.bl_static_type == 'REROUTE':
            return node.location_absolute.y
        elif node.bl_static_type == 'FRAME':
            return min(self.get_bottom(node) for node in self.frame_children(node)) - frame_margin
        elif node.hide:
            return node.location_absolute.y - (0.5 * self.get_height(node)) - weird_offset
        else:
            return node.location_absolute.y - self.get_height(node)

    def get_bounds(self, nodes):
        min_x = min(self.get_left(node) for node in nodes)
        max_x = max(self.get_right(node) for node in nodes)
        min_y = min(self.get_bottom(node) for node in nodes)
        max_y = max(self.get_top(node) for node in nodes)

        return min_x, max_x, min_y, max_y
    
    def move_children(self, frame, offset, axis):
        children = self.frame_children(frame)
        
        for node in children:
            if node.bl_static_type == 'FRAME':
                self.move_children(node, offset, axis)
            else:
                if axis == 'X':
                    node.location_absolute.x += offset
                else:
                    node.location_absolute.y += offset

    def arrange_nodes(self, nodes):
        margin = self.margin

        # Check if nodes should be laid out horizontally or vertically
        # use dimension to get center of node, not corner
        x_locs = [self.get_center(n) for n in nodes]
        y_locs = [self.get_middle(n) for n in nodes]
        x_range = max(x_locs) - min(x_locs)
        y_range = max(y_locs) - min(y_locs)
        mid_x = (max(x_locs) + min(x_locs)) / 2
        mid_y = (max(y_locs) + min(y_locs)) / 2
        horizontal = x_range > y_range

        # Sort selection by location of node mid-point
        if horizontal:
            nodes = sorted(nodes, key=self.get_center)
        else:
            nodes = sorted(nodes, key=self.get_middle, reverse=True)

        # Alignment
        current_pos = 0
        for i, node in enumerate(nodes):

            current_margin = 0

            # Use a smaller margin for hidden nodes.
            current_margin = current_margin * 0.5 if node.hide else current_margin

            if horizontal:
                print(node, node.location_absolute)
                if i > 0:
                    target_x = current_pos
                    if node.bl_idname == "NodeFrame":
                        self.move_children(node, target_x - node.location_absolute.x, axis="X")
                    else:
                        node.location_absolute.x = target_x

                if i == 0:
                    current_pos += self.get_left(node)
                current_pos += current_margin + self.get_width(node)
                
                target_y = mid_y + (self.get_height(node) / 2)
                if node.bl_idname == "NodeFrame":
                    self.move_children(node, target_y - node.location_absolute.y, axis="Y")
                else:
                    node.location_absolute.y = target_y
            else:
                # `node.bl_height_min` is the min size of a collapsed node, +6 for the outlines and margins.
                hide_offset = (self.get_height(node) - (node.bl_height_min + 6)) / 2 if node.hide else 0
                
                if i > 0:
                    # Hidden nodes center their sockets around the label instead of below.
                    target_y = current_pos - hide_offset
                    if node.bl_idname == "NodeFrame":
                        self.move_children(node, target_y - node.location_absolute.y, axis="Y")
                    node.location_absolute.y = target_y
                
                if i == 0:
                    current_pos += node.location_absolute.y
                # Use half-margin for vertical alignment.
                current_pos -= (current_margin * 0.3) + self.get_height(node)

                target_x = mid_x - (self.get_width(node) / 2)
                if node.bl_idname == "NodeFrame":
                    self.move_children(node, target_x - node.location_absolute.x, axis="X")
                node.location_absolute.x = target_x

    def execute(self, context):
        nodes = context.selected_nodes
        parent_map = {}
        self.tree = context.space_data.edit_tree

        for node in nodes:
            children = parent_map.get(node.parent, None)
            if children is None:
                parent_map[node.parent] = []

            parent_map[node.parent].append(node)

        self.parent_map = parent_map

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
