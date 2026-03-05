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
    
    # TODO - Make this take in a single tuple
    def move_children(self, frame, offset, axis):
        children = self.frame_children(frame)

        if -1.0 < offset < 1.0:
            return

        for node in children:
            if node.bl_static_type == 'FRAME':
                self.move_children(node, offset, axis)
            else:
                if axis == 'X':
                    node.location_absolute.x += offset
                else:
                    node.location_absolute.y += offset

    def arrange_nodes(self, nodes):
        min_x, max_x, min_y, max_y = self.get_bounds(nodes)
        mid_x = (max_x + min_x) / 2
        mid_y = (max_y + min_y) / 2

        x_locs = [self.get_center(n) for n in nodes]
        y_locs = [self.get_middle(n) for n in nodes]
        horizontal = max(x_locs) - min(x_locs) > max(y_locs) - min(y_locs)

        if horizontal:
            nodes = sorted(nodes, key=self.get_center)
        else:
            nodes = sorted(nodes, key=self.get_middle, reverse=True)

        # Alignment
        if horizontal:
            current_pos = min_x
            margin = self.margin

            for i, node in enumerate(nodes):
                if i > 0:
                    target_x = current_pos
                    if node.bl_idname == "NodeFrame":
                        self.move_children(node, target_x - node.location_absolute.x, axis="X")
                    else:
                        node.location_absolute.x = target_x
                
                current_pos += margin + self.get_width(node)
                
                if node.bl_idname == "NodeFrame":
                    self.move_children(node, mid_y + (self.get_height(node) / 2) - node.location_absolute.y, axis="Y")
                elif node.hide:
                    node.location_absolute.y = mid_y + weird_offset
                else:
                    node.location_absolute.y = mid_y + (self.get_height(node) / 2)
        else:
            current_pos = max_y
            # Use a smaller margin for hidden nodes.
            margin = 0.5 * self.margin

            for i, node in enumerate(nodes):
                if i > 0:
                    if node.bl_idname == "NodeFrame":
                        self.move_children(node, current_pos - node.location_absolute.y, axis="Y")
                    elif node.hide:
                        node.location_absolute.y = current_pos + (-0.5 * self.get_height(node)) + weird_offset
                    else:
                        node.location_absolute.y = current_pos
                
                # Use half-margin for vertical alignment.
                current_pos -= margin + self.get_height(node)

                target_x = mid_x - (self.get_width(node) / 2)
                if node.bl_idname == "NodeFrame":
                    self.move_children(node, target_x - node.location_absolute.x, axis="X")
                else:
                    node.location_absolute.x = target_x

    def execute(self, context):
        nodes = context.selected_nodes
        active_node = context.active_node
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
        
        if active_node is not None:
            old_x, old_y = self.get_left(active_node), self.get_top(active_node)
        else:
            min_x, max_x, min_y, max_y = self.get_bounds(tuple((n for n in nodes if (n.bl_idname != "NodeFrame"))))
            old_x = (max_x + min_x) / 2
            old_y = (max_y + min_y) / 2

        sorted_keys = sorted(parent_map.keys(), key=self.parent_depth, reverse=True)
        for parent in sorted_keys:
            children = parent_map[parent]
            
            # When a frame's children get moved, this introduces a shift in where the frame is visually positioned.
            # If said frame is going to be arranged with other frames/nodes, compensate for this shift to keep
            # location calculations accurate. 

            # Otherwise, allow for this to freely happen as it feels more natural
            # in cases where the frame isn't part of the selection.
            should_anchor = getattr(parent, "select", False) and (len(parent_map.get(parent.parent, ())) > 1)

            if should_anchor:
                old_left, old_top = self.get_left(parent), self.get_top(parent)
                
            self.arrange_nodes(children)

            if should_anchor:
                self.move_children(parent, offset=old_left - self.get_left(parent), axis="X")
                self.move_children(parent, offset=old_top - self.get_top(parent), axis="Y")
        
        if active_node is not None:
            new_x = self.get_left(active_node) 
            new_y = self.get_top(active_node)
        else:
            min_x, max_x, min_y, max_y = self.get_bounds(tuple((n for n in nodes if (n.bl_idname != "NodeFrame"))))
            new_x = (max_x + min_x) / 2
            new_y = (max_y + min_y) / 2

        offset_x = old_x - new_x
        offset_y = old_y - new_y

        for node in nodes:
            node.location_absolute.x += offset_x
            node.location_absolute.y += offset_y

        return {'FINISHED'}
