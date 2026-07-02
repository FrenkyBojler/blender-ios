# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import (
    Panel, Menu
)


class NODE_MT_add_scene_compositor_modifier(Menu):
    bl_label = "Add Modifier"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout

        if layout.operator_context == 'EXEC_REGION_WIN':
            layout.operator_context = 'INVOKE_REGION_WIN'
            layout.operator(
                "WM_OT_search_single_menu",
                text="Search...",
                icon='VIEWZOOM',
            ).menu_idname = "NODE_MT_add_scene_compositor_modifier_add"
            layout.separator()

        layout.operator_context = 'INVOKE_REGION_WIN'

        layout.operator("node.add_scene_compositor_modifier", text="Add Modifier", icon='ADD')
        layout.menu_contents("NODE_MT_add_scene_compositor_modifier_root_catalogs")


class SCENE_PT_compositor_modifiers(Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "scene_compositor_modifiers"
    bl_label = "Modifiers"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        layout.operator("wm.call_menu", text="Add Modifier", icon='ADD').name = "NODE_MT_add_scene_compositor_modifier"

        layout.template_scene_compositor_modifiers()


classes = (
    SCENE_PT_compositor_modifiers,
    NODE_MT_add_scene_compositor_modifier,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
