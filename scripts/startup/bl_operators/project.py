# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import tomllib

import bpy
from bpy.types import Operator


PROJECT_DIR = ".blender_project"
PROJECT_CONFIG = "project.toml"


class PROJECT_OP_NewProject(Operator):
    """Create a new project"""
    bl_idname = "project.new_project"
    bl_label = "New Project"

    directory: bpy.props.StringProperty(
        name="Project Root",
        subtype='DIR_PATH',
        default="",
    )

    filter_folder: bpy.props.BoolProperty(
        name="Filter folders",
        default=True,
        options={'HIDDEN'},
    )

    @classmethod
    def poll(cls, context):
        return context.project.data is None

    def execute(self, context):
        # TODO: validate `self.directory`.
        context.project.init("New Project", self.directory)
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}


class PROJECT_OP_WriteProject(Operator):
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

        context.project.is_dirty = False

        return {'FINISHED'}


# -----------------------------------------------------------------------------
# Auto-loading / clearing of projects when loading/saving blend files.

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
    if bpy.context.project.data is not None and bpy.context.project.is_dirty:
        # TODO: make this conditional on auto-save being enabled or not.
        bpy.ops.project.write_project()

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

    bpy.context.project.is_dirty = False


@bpy.app.handlers.persistent
def on_blend_save(blend_path: str) -> None:
    """ This is needed due to cases like a fresh new blend file being saved for
        the first time in a project.
    """

    if bpy.context.project.data is not None:
        return

    on_blend_load(blend_path)


# -----------------------------------------------------------------------------
# Register

classes = (
    PROJECT_OP_NewProject,
    PROJECT_OP_WriteProject
)


def register():
    bpy.app.handlers.load_pre.append(on_blend_load)
    bpy.app.handlers.save_post.append(on_blend_save)


def unregister():
    bpy.app.handlers.load_pre.remove(on_blend_load)
    bpy.app.handlers.save_post.append(on_blend_save)
