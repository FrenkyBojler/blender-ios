# SPDX-FileCopyrightText: 2009-2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Operator
from bpy.props import (
    EnumProperty,
)


class GREASE_PENCIL_OT_relative_layer_mask_add(Operator):
    """Mask active layer with above or below"""

    bl_idname = "grease_pencil.relative_layer_mask_add"
    bl_label = "Mask with Above/Below"
    bl_options = {'REGISTER', 'UNDO'}

    mode: EnumProperty(
        name="Mode",
        items=(
            ('ABOVE', "Above", ""),
            ('BELOW', "Below", "")
        ),
        description="Mask active layer with above or below",
        default='ABOVE',
    )

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj is not None and obj.is_editable and obj.data.layers.active is not None

    def execute(self, context):
        obj = context.active_object
        active_layer = obj.data.layers.active

        masking_layer = (active_layer.prev_node, active_layer.next_node)[self.mode == 'ABOVE']

        if (masking_layer is None or type(masking_layer) != bpy.types.GreasePencilLayer):
            self.report({'ERROR'}, "No node found")
            return {'CANCELLED'}

        bpy.ops.grease_pencil.layer_mask_add(name=masking_layer.name)
        active_layer.use_masks = True
        return {'FINISHED'}


classes = (
    GREASE_PENCIL_OT_relative_layer_mask_add,
)
