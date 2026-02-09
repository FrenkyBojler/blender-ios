# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Header, Menu, Panel

from bpy.app.translations import pgettext_iface

from .space_userpref import CenterAlignMixIn


MAIN_SECTION_NAME = "General"


# -------------------------------------------------------------
# Header.

class PROJECT_HT_header(Header):
    bl_space_type = 'PROJECT'

    def draw(self, context):
        if not bpy.context.preferences.experimental.use_blender_projects:
            return

        layout = self.layout

        layout.template_header()
        PROJECT_MT_editor_menus.draw_collapsible(context, layout)
        layout.separator_spacer()


class PROJECT_MT_editor_menus(Menu):
    bl_idname = "PROJECT_MT_editor_menus"
    bl_label = ""

    def draw(self, context):
        if not bpy.context.preferences.experimental.use_blender_projects:
            return

        layout = self.layout
        layout.menu("PROJECT_MT_view")
        layout.menu("PROJECT_MT_save_load", text="Project")


class PROJECT_MT_view(Menu):
    bl_label = "View"

    def draw(self, context):
        if not bpy.context.preferences.experimental.use_blender_projects:
            return

        layout = self.layout
        project_space = context.space_data

        layout.prop(project_space, "show_region_ui")

        layout.separator()

        layout.menu("INFO_MT_area")


class PROJECT_MT_save_load(Menu):
    bl_label = "Save & Load"

    def draw(self, context):
        if not bpy.context.preferences.experimental.use_blender_projects:
            return

        layout = self.layout
        project_space = context.space_data

        prefs = context.preferences

        layout.prop(prefs, "use_project_auto_save", text="Auto-Save Project")
        layout.operator("project.save_project", text="Save Project")


# -------------------------------------------------------------
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
        if not bpy.context.preferences.experimental.use_blender_projects:
            return

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
                icon='FILE_TICK',
                translate=False,
            )


# -------------------------------------------------------------
# Navigation Bar.

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
        if not bpy.context.preferences.experimental.use_blender_projects:
            return

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


# -------------------------------------------------------------
# Main Area.

class PROJECT_PT_main(Panel, CenterAlignMixIn):
    bl_label = "Project"
    bl_space_type = 'PROJECT'
    bl_region_type = 'WINDOW'
    bl_category = MAIN_SECTION_NAME

    @classmethod
    def poll(cls, context):
        return context.project and context.project.data

    def centered_operator(self, layout, op_name, text=None, icon=None):
        col_flow = layout.column_flow(columns=3)
        col_flow.separator_spacer()
        col_flow.operator(op_name, text=text, icon=icon)
        col_flow.separator_spacer()

    def draw_centered(self, context, layout):
        if not bpy.context.preferences.experimental.use_blender_projects:
            return

        project = context.project

        col = layout.column()
        col.prop(project.data, "name")
        col.prop(project.data, "root_path")


class PROJECT_PT_main_unset(Panel, CenterAlignMixIn):
    bl_label = "No Project"
    bl_space_type = 'PROJECT'
    bl_region_type = 'WINDOW'
    bl_options = {'HIDE_HEADER'}
    bl_category = MAIN_SECTION_NAME

    @classmethod
    def poll(cls, context):
        return not PROJECT_PT_main.poll(context)

    def draw_centered(self, context, layout):
        if not bpy.context.preferences.experimental.use_blender_projects:
            return

        col = layout.column()
        col.separator(factor=2.0)

        if context.blend_data.filepath == "":
            row = col.row()
            row.alignment = 'CENTER'
            row.label(
                text="No active project.",
                icon="INFO",
            )
            col.separator()

            row = col.row()
            row.alignment = 'CENTER'
            row.label(text="Save the current file, and make sure to place it in a folder that will be part of the project")

            row = col.row()
            row.alignment = 'CENTER'
            row.label(text="Alternatively, open a file inside of a project directory to see its settings.")

            col.separator()
            row = col.row()
            row.alignment = 'CENTER'
            row.operator("wm.save_as_mainfile", text="Save File...", icon='FILE_TICK')
            row.operator("project.open_blend_in_project", icon='FILE_FOLDER')
        else:
            row = col.row()
            row.alignment = 'CENTER'
            row.label(
                text="No active project.",
                icon="INFO",
            )
            col.separator()

            row = col.row()
            row.alignment = 'CENTER'
            row.label(text="Set up a new project by choosing any parent directory of the current file.")
            row = col.row()
            row.alignment = 'CENTER'
            row.label(text="Alternatively, open a file inside of a project directory to see its settings.")

            col.separator()
            row = col.row()
            row.alignment = 'CENTER'
            row.operator("project.new_project", text="New Project...", icon='ADD')
            row.operator("project.open_blend_in_project", icon='FILE_FOLDER')


class PROJECT_UL_variables(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname):
        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            col = layout.column()
            col.prop(item, "name")

            col = layout.column()
            col.prop(item, "type")

            col = layout.column()
            col.alignment = 'RIGHT'
            match item.type:
                case 'INTEGER':
                    col.prop(item, "value_int")
                case 'FLOAT':
                    col.prop(item, "value_float")
                case 'STRING':
                    col.prop(item, "value_string")
                case 'FILEPATH':
                    col.prop(item, "value_string")
        # 'GRID' layout type should be as compact as possible (typically a single icon!).
        elif self.layout_type in {'GRID'}:
            # TODO
            pass


class PROJECT_PT_variables(Panel, CenterAlignMixIn):
    bl_label = "Variables"
    bl_space_type = 'PROJECT'
    bl_region_type = 'WINDOW'
    bl_category = "Variables"

    @classmethod
    def poll(cls, context):
        return context.project and context.project.data

    def draw_centered(self, context, layout):
        if not bpy.context.preferences.experimental.use_blender_projects:
            return

        project = context.project

        row = layout.row()

        row.template_list(
            listtype_name="PROJECT_UL_variables",
            list_id="Variables",
            dataptr=project.data,
            propname="variables",
            active_dataptr=project.data,
            active_propname="active_variable",
        )


# -------------------------------------------------------------
# Register

# This conditional is awkward: it means the user has to restart Blender after
# enabling the experimental feature to actually get access to the UI.
#
# However, this seems to be the only way to handle things in the current system
# for experimental features if we want to hide the project space type in
# non-experimental builds. If we don't do this, then there are Python errors
# when loading Blender due to `bl_space_type = 'PROJECT'` in the UI classes.
#
# Would love another way to do this!
if bpy.context.preferences.experimental.use_blender_projects:
    classes = (
        PROJECT_HT_header,
        PROJECT_MT_editor_menus,
        PROJECT_MT_view,
        PROJECT_MT_save_load,
        PROJECT_PT_navigation_bar,
        PROJECT_PT_save_project,
        PROJECT_PT_main_unset,
        PROJECT_PT_main,
        PROJECT_PT_variables,
        PROJECT_UL_variables,
    )
else:
    classes = ()
