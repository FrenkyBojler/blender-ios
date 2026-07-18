# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
N-panel UI for the mode (Brush + Dyntopo + Multires). Panels poll on the mode
being active so they never fight vanilla sculpt panels; they read the shared
``tool_settings.sculpt`` brush (brush-mapping decision 1). The Multires panel
exposes the modifier's ``sculpt_levels``, which the depsgraph handler mirrors
into the engine's active level (P8 C2).
"""

import bpy

from . import engine, mapping, multires

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

        # Size/strength route to the unified settings when those own the
        # value (what the stroke and cursor read); the lock toggles switch
        # ownership like vanilla paint panels.
        unified = context.tool_settings.sculpt.unified_paint_settings
        col = layout.column()
        col.active = supported
        row = col.row(align=True)
        row.prop(unified if unified.use_unified_size else brush,
                 "size", text="Radius", slider=True)
        row.prop(unified, "use_unified_size", text="", icon='WORLD')
        row = col.row(align=True)
        row.prop(unified if unified.use_unified_strength else brush,
                 "strength")
        row.prop(unified, "use_unified_strength", text="", icon='WORLD')
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


class SCULPTCORE_PT_multires(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = _CATEGORY
    bl_label = "Multires"

    @classmethod
    def poll(cls, context):
        if not _in_mode(context):
            return False
        session = engine.sessions.get(context.active_object.name)
        return session is not None and session.multires_ptr is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        ob = context.active_object
        md = multires.modifier(ob)
        if md is None:
            return
        # The handler follows this property and switches the engine level.
        layout.prop(md, "sculpt_levels", text="Sculpt Level")
        session = engine.sessions.get(ob.name)
        if session is not None and md.sculpt_levels < 1:
            layout.label(text="Level 0 sculpts at level 1", icon='INFO')


_classes = (
    SCULPTCORE_PT_brush,
    SCULPTCORE_PT_dyntopo,
    SCULPTCORE_PT_multires,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
