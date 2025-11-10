# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import tomllib

import bpy
from bpy.types import Header, Menu, Panel

# TODO: move most of this stuff to the Blender Projects add-on.

PROJECT_DIR = ".blender_project"
PROJECT_CONFIG = "project.toml"


class PROJECT_OP_NewProject(bpy.types.Operator):
    """Create a new project"""
    bl_idname = "project.new_project"
    bl_label = "New Project"

    @classmethod
    def poll(cls, context):
        return context.project.data is None

    def execute(self, context):
        context.project.init("New Project", "/my_project")
        return {'FINISHED'}


class PROJECT_OP_WriteProject(bpy.types.Operator):
    """Write the current project to disk"""
    bl_idname = "project.write_project"
    bl_label = "Write Project"

    @classmethod
    def poll(cls, context):
        return context.project.data is not None

    def execute(self, context):
        # TODO: this is just a quick-and-dirty version of this. No proper error
        # handling, etc.

        data = context.project.data
        root_path = Path(data.root_path)

        if not root_path.is_absolute():
            print("Can't write project to non-absolute path.")
            return {'CANCELLED'}

        root_path.mkdir(parents=True, exist_ok=True)

        config_dir_path = root_path.joinpath(PROJECT_DIR)
        config_dir_path.mkdir(parents=True, exist_ok=True)

        config_path = root_path.joinpath(PROJECT_DIR, PROJECT_CONFIG)
        with config_path.open(mode='w', encoding='utf-8') as f:
            f.write("name = \"{}\"\n".format(data.name))

        return {'FINISHED'}


# -----------------------------------------------------------------------------
# Header

class PROJECT_HT_header(Header):
    bl_space_type = 'PROJECT'

    def draw(self, context):
        layout = self.layout

        layout.template_header()
        PROJECT_MT_editor_menus.draw_collapsible(context, layout)
        layout.separator_spacer()


class PROJECT_MT_editor_menus(bpy.types.Menu):
    bl_idname = "PROJECT_MT_editor_menus"
    bl_label = ""

    def draw(self, context):
        del context
        layout = self.layout
        layout.menu("PROJECT_MT_view")


class PROJECT_MT_view(bpy.types.Menu):
    bl_label = "View"

    def draw(self, context):
        layout = self.layout
        project_space = context.space_data

        layout.prop(project_space, "show_region_toolbar")
        layout.prop(project_space, "show_region_ui")

        layout.separator()

        layout.prop(project_space, "show_internal_attributes", text="Internal Attributes")

        layout.separator()

        layout.menu("INFO_MT_area")

# -----------------------------------------------------------------------------
# Navigation Bar


class PROJECT_PT_navigation_bar(Panel):
    bl_label = "Project Navigation"
    bl_space_type = 'PROJECT'
    bl_region_type = 'NAVIGATION_BAR'
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout

        space_data = context.space_data

        col = layout.column()

        col.scale_x = 1.3
        col.scale_y = 1.3
        col.prop(space_data, "active_section", expand=True)

# -----------------------------------------------------------------------------
# Main Area


class PROJECT_PT_main(Panel):
    bl_label = "Project"
    bl_space_type = 'PROJECT'
    bl_region_type = 'WINDOW'
    bl_options = {'HIDE_HEADER'}

    @classmethod
    def poll(cls, context):
        return True

    def draw(self, context):
        layout = self.layout
        project = context.project

        col = layout.column()

        if context.project.data is None:
            col.label(text="No project!", icon='INFO')
            col.operator("project.new_project")
        else:
            col.prop(project.data, "name")
            col.prop(project.data, "root_path")
            col.operator("project.write_project")


classes = (
    PROJECT_HT_header,
    PROJECT_MT_editor_menus,
    PROJECT_MT_view,
    PROJECT_PT_navigation_bar,
    PROJECT_PT_main,
    PROJECT_OP_NewProject,
    PROJECT_OP_WriteProject
)

# --------------------------------------------------------------
# EVERYTHING BELOW SHOULD PROBABLY GO SOMEWHERE ELSE...?

PROJECT_DIR = ".blender_project"
PROJECT_CONFIG = "project.toml"


def find_project_root_from_blend_file_path(blend_path: Path) -> Path | None:
    for parent in blend_path.parents:
        if parent.joinpath(PROJECT_DIR).is_dir():
            return parent
    return None


def read_project_config(root_path: Path) -> dict | None:
    name = "My Project"

    config_path = root_path.joinpath(PROJECT_DIR, PROJECT_CONFIG)
    try:
        with open(config_path, "rb") as f:
            return tomllib.load(f)
    except FileNotFoundError:
        return None


@bpy.app.handlers.persistent
def on_blend_load(blend_path: str) -> None:
    bpy.context.project.clear()

    if blend_path == "":
        # Not an on-disk blend file.
        return

    root_path = find_project_root_from_blend_file_path(Path(blend_path))
    if root_path is None:
        return

    config = read_project_config(root_path)
    if config is None:
        print("Invalid project: no 'project.toml' found.")
        return

    if "name" not in config:
        print("Invalid project: no project name defined in 'project.toml'.")
        return

    if type(config["name"]) != str:
        print("Invalid project: project name is not a string.")
        return

    if config["name"] == "":
        print("Invalid project: project name is empty.")
        return

    bpy.context.project.init(config["name"], str(root_path))


@bpy.app.handlers.persistent
def on_blend_save(blend_path: str) -> None:
    # This is needed due to cases like a fresh new blend file being saved for
    # the first time in a project.
    on_blend_load(blend_path)


####################
# REGISTER

def register():
    bpy.app.handlers.load_pre.append(on_blend_load)
    bpy.app.handlers.save_post.append(on_blend_save)


def unregister():
    bpy.app.handlers.load_pre.remove(on_blend_load)
    bpy.app.handlers.save_post.append(on_blend_save)
