# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy


class LIGHTMANAGER_HT_header(bpy.types.Header):
    bl_space_type = 'LIGHT_MANAGER'

    def draw(self, context):
        layout = self.layout
        space = context.space_data

        # Standard editor header: editor selector, menus, etc.
        layout.template_header()

        # Collapsible menus container (kept minimal for now).
        LIGHTMANAGER_MT_editor_menus.draw_collapsible(context, layout)
        layout.separator_spacer()

        # Sorting controls.
        row = layout.row(align=True)
        row.label(text="Sort")
        row.prop(space, "sort_type", text="", expand=False)


class LIGHTMANAGER_MT_editor_menus(bpy.types.Menu):
    bl_idname = "LIGHTMANAGER_MT_editor_menus"
    bl_label = ""

    def draw(self, context):
        layout = self.layout
        # For now, just expose the generic area menu (full-screen, duplicate area, etc.).
        layout.menu("INFO_MT_area")


classes = (
    LIGHTMANAGER_HT_header,
    LIGHTMANAGER_MT_editor_menus,
)
