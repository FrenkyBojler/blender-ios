# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import (
    Panel,
)


class SCENE_PT_compositor_modifiers(Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "scene_compositor_modifiers"
    bl_label = "Modifiers"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        layout.operator("node.add_scene_compositor_modifier", text="Add Modifier", icon='ADD')
        #layout.operator("wm.call_menu", text="Add Modifier", icon='ADD').name = "SEQUENCER_MT_modifier_add"

        layout.template_scene_compositor_modifiers()


classes = (
    SCENE_PT_compositor_modifiers,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)

