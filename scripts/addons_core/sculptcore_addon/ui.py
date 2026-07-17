# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
N-panel UI for the mode (Brush + Dyntopo). Panels poll on the mode being
active so they never fight vanilla sculpt panels; they read the shared
``tool_settings.sculpt`` brush (brush-mapping decision 1).
"""

import bpy

from . import mapping

_CATEGORY = "SculptCore"


def _in_mode(context):
    ob = context.active_object
    return (
        ob is not None
        and ob.mode == 'CUSTOM'
        and ob.custom_mode == "sculptcore.sculpt"
    )


class SCULPTCORE_PT_brush(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = _CATEGORY
    bl_label = "Brush"

    @classmethod
    def poll(cls, context):
        return _in_mode(context)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        brush = context.tool_settings.sculpt.brush
        if brush is None:
            layout.label(text="No active brush", icon='INFO')
            return

        supported = brush.sculpt_brush_type in mapping.KERNEL_BY_TYPE
        row = layout.row()
        row.prop(brush, "sculpt_brush_type", text="Type")
        if not supported:
            box = layout.box()
            box.label(text="Brush type not yet mapped", icon='ERROR')

        col = layout.column()
        col.active = supported
        col.prop(brush, "size", text="Radius")
        col.prop(brush, "strength")
        col.prop(brush, "spacing")
        col.prop(brush, "direction", expand=True)


class SCULPTCORE_PT_dyntopo(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = _CATEGORY
    bl_label = "Dyntopo"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return _in_mode(context)

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        layout.prop(scene, "sculptcore_dyntopo", text="Dynamic Topology")
        col = layout.column()
        col.use_property_split = True
        col.active = scene.sculptcore_dyntopo
        col.prop(scene, "sculptcore_detail")


_classes = (
    SCULPTCORE_PT_brush,
    SCULPTCORE_PT_dyntopo,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
