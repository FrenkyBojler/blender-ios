# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Header, Menu, Panel

from bpy.app.translations import pgettext_iface


MAIN_SECTION_NAME = "General"


# -----------------------------------------------------------------------------
# Header

class PROJECT_HT_header(Header):
    bl_space_type = 'PROJECT'

    def draw(self, context):
        layout = self.layout

        layout.template_header()
        PROJECT_MT_editor_menus.draw_collapsible(context, layout)
        layout.separator_spacer()


class PROJECT_MT_editor_menus(Menu):
    bl_idname = "PROJECT_MT_editor_menus"
    bl_label = ""

    def draw(self, context):
        del context
        layout = self.layout
        layout.menu("PROJECT_MT_view")
        layout.menu("PROJECT_MT_save_load", text="Project")


class PROJECT_MT_view(Menu):
    bl_label = "View"

    def draw(self, context):
        layout = self.layout
        project_space = context.space_data

        layout.prop(project_space, "show_region_ui")

        layout.separator()

        layout.menu("INFO_MT_area")


class PROJECT_MT_save_load(Menu):
    bl_label = "Save & Load"

    def draw(self, context):
        layout = self.layout
        project_space = context.space_data

        prefs = context.preferences

        layout.prop(prefs, "use_project_auto_save", text="Auto-Save Project")
        layout.operator("project.save_project", text="Save Project")


# -----------------------------------------------------------------------------
# Execution area (shown when header is hidden).

class PROJECT_PT_save_project(Panel):
    bl_label = "Save Project"
    bl_space_type = 'PROJECT'
    bl_region_type = 'EXECUTE'
    bl_options = {'HIDE_HEADER'}

    @classmethod
    def poll(cls, context):
        return True

    def draw(self, context):
        layout = self.layout.row()
        layout.operator_context = 'EXEC_AREA'

        layout.menu("PROJECT_MT_save_load", text="", icon='COLLAPSEMENU')

        # Save button.
        if not context.preferences.use_project_auto_save and context.project.data is not None:
            # Show '*' to let users know the project has been modified.
            # It is shown to the left so that it is visible when the sidebar is narrow,
            # and for consistency with unsaved files in the title bar.
            layout.operator(
                "project.save_project",
                text=("* " if context.project.is_dirty else "") + pgettext_iface("Save Project"),
                translate=False,
            )


# -----------------------------------------------------------------------------
# Navigation Bar

class PROJECT_PT_navigation_bar(Panel):
    bl_label = "Project Navigation"
    bl_space_type = 'PROJECT'
    bl_region_type = 'UI'
    bl_category = "Navigation"
    bl_options = {'HIDE_HEADER'}

    @classmethod
    def poll(cls, context):
        return True

    def draw(self, context):
        layout = self.layout

        space_data = context.space_data

        col = layout.column()

        if context.project.data is None:
            # If there's no project, we need to make sure the UI for creating a
            # new project is visible. That UI is in the main section, so we
            # ensure it's the active section.
            space_data.active_section = MAIN_SECTION_NAME
            col.enabled = False

        col.scale_x = 1.3
        col.scale_y = 1.3
        col.prop(space_data, "active_section", expand=True)


# -----------------------------------------------------------------------------
# Main Area

# Panel mix-in, copied from `space_userpref.py`.
#
# TODO: we have this in at least two places now.  Should this be built-in UI
# functionality?
class CenterAlignMixIn:
    """
    Base class for panels to center align contents with some horizontal margin.
    Deriving classes need to implement a ``draw_centered(context, layout)`` function.
    """

    def draw(self, context):
        layout = self.layout
        width = context.region.width
        ui_scale = context.preferences.system.ui_scale
        # No horizontal margin if region is rather small.
        is_wide = width > (350 * ui_scale)

        layout.use_property_split = True
        layout.use_property_decorate = False  # No animation.

        row = layout.row()
        if is_wide:
            row.label()  # Needed so col below is centered.

        col = row.column()
        col.ui_units_x = 50

        # Implemented by sub-classes.
        self.draw_centered(context, col)

        if is_wide:
            row.label()  # Needed so col above is centered.


class PROJECT_PT_main(Panel, CenterAlignMixIn):
    bl_label = "Project"
    bl_space_type = 'PROJECT'
    bl_region_type = 'WINDOW'
    bl_options = {'HIDE_HEADER'}
    bl_category = MAIN_SECTION_NAME

    @classmethod
    def poll(cls, context):
        return True

    def draw_centered(self, context, layout):
        project = context.project

        col = layout.column()

        if context.project.data is None:
            col.label(text="No active project.", icon='INFO')

            col.label(text="Lorem ipsum dolor sit amet, consectetur adipiscing elit. Quisque sit amet")
            col.label(text="mi et magna mattis faucibus. Sed aliquet mi justo.")

            row = col.row()
            split = row.split(factor=0.3)
            split.operator("project.new_project")
        else:
            col.prop(project.data, "name")
            col.prop(project.data, "root_path")


# -----------------------------------------------------------------------------
# Register

classes = (
    PROJECT_HT_header,
    PROJECT_MT_editor_menus,
    PROJECT_MT_view,
    PROJECT_MT_save_load,
    PROJECT_PT_navigation_bar,
    PROJECT_PT_save_project,
    PROJECT_PT_main,
)
