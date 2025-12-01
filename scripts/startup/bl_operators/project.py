# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import tomllib
import atexit

import bpy
from bpy.types import Operator


PROJECT_DIR = ".blender_project"
PROJECT_CONFIG = "project.toml"


# --------------------------------------------------------------
# Custom exception types, used for reporting expected errors.
#
# Mainly used so we can raise, catch, and report expected errors to the user.

class ProjectSaveException(Exception):
    pass


class ProjectLoadException(Exception):
    pass


# --------------------------------------------------------------

def save_project(project):
    """ Note: throws a ProjectSaveException on anticipated errors.
        Other exceptions indicate unanticipated errors (a.k.a. bugs).
    """

    print("Saving project '{}' at '{}'...".format(project.data.name, project.data.root_path))

    data = project.data
    root_path = Path(data.root_path)

    if not root_path.is_absolute():
        raise ProjectSaveException("Can't write project to non-absolute path.")

    if not root_path.is_dir():
        raise ProjectSaveException("Project root directory does not exist.")

    config_dir_path = root_path.joinpath(PROJECT_DIR)

    try:
        config_dir_path.mkdir(parents=True, exist_ok=True)
    except FileExistsError:
        raise ProjectSaveException(
            "A file named '{}' already exists, but it needs to be a directory.".format(PROJECT_DIR))
    except PermissionError:
        raise ProjectSaveException("Cannot create '{}' directory due to filesystem permissions.".format(PROJECT_DIR))

    config_path = root_path.joinpath(PROJECT_DIR, PROJECT_CONFIG)
    try:
        with config_path.open(mode='w', encoding='utf-8') as f:
            # The actual project file writing.
            f.write("name = \"{}\"\n".format(data.name))
    except PermissionError:
        raise ProjectSaveException("Cannot write to '{}' due to filesystem permissions.".format(PROJECT_CONFIG))

    project.is_dirty = False

    print("...done.")


def find_project_root_from_blend_file_path(blend_path: Path) -> Path | None:
    """ Searches for a Blender project root in the parent directories of the
        given path.

        Returns the project root if found, or None otherwise.
    """

    for parent in blend_path.parents:
        if parent.joinpath(PROJECT_DIR).is_dir():
            return parent
    return None


def read_project_toml_config(root_path: Path) -> dict:
    """ Note: throws a ProjectLoadException on anticipated errors.
        Other exceptions indicate unanticipated errors (a.k.a. bugs).
    """

    config_path = root_path.joinpath(PROJECT_DIR, PROJECT_CONFIG)
    try:
        with open(config_path, "rb") as f:
            return tomllib.load(f)
    except FileNotFoundError:
        raise ProjectLoadException("Project has no {} file.".format(PROJECT_CONFIG))
    except PermissionError:
        raise ProjectLoadException("Cannot access {} file due to filesystem permissions.".format(PROJECT_CONFIG))


def validate_config(config: dict):
    """ Note: throws a ProjectLoadException if there's a validation error.
    """
    if "name" not in config:
        raise ProjectLoadException("Invalid project: no project name defined in '{}'.".format(PROJECT_CONFIG))
        return

    if type(config["name"]) != str:
        raise ProjectLoadException("Invalid project: project name is not a string.")
        return

    if config["name"] == "":
        raise ProjectLoadException("Invalid project: project name is empty.")
        return


def load_project_for_blend_path(context, blend_path: str):
    """ Loads the project for the given blend file path, or clears the project
        if no such project is found.

        Note: throws a ProjectLoadException on anticipated errors. Other
        exceptions indicate unanticipated errors (a.k.a. bugs).
    """

    if blend_path == "":
        # Not an on-disk blend file, so there is no project to load.
        context.project.clear()
        return

    root_path = find_project_root_from_blend_file_path(Path(blend_path))
    if root_path is None:
        # No project.
        context.project.clear()
        return

    if context.project.data is not None and root_path == context.project.data.root_path:
        # We already have this project loaded, and we don't want to obliterate
        # local unsaved changes if auto-save isn't turned on.
        return

    # Load project.
    config = read_project_toml_config(root_path)
    if config is None:
        raise ProjectLoadException("Invalid project: no '{}' found.".format(PROJECT_CONFIG))

    validate_config(config)

    context.project.clear()

    context.project.init(config["name"], str(root_path))

    context.project.is_dirty = False


# --------------------------------------------------------------

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
        # TODO: ensure there isn't already a project at `self.directory`.
        #
        # TODO: ensure `self.directory` is a parent of the current file (if the
        # file is on disk).

        # Create the project.
        context.project.init("New Project", self.directory)

        # Immediately save the project.
        try:
            save_project(context.project)
        except ProjectSaveException as e:
            self.report({'ERROR'}, "Failed to save project: {}".format(e))
            return {'CANCELLED'}

        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}


class PROJECT_OP_SaveProject(Operator):
    """Save the current project to disk"""
    bl_idname = "project.save_project"
    bl_label = "Save Project"

    @classmethod
    def poll(cls, context):
        return context.project.data is not None

    def execute(self, context):
        try:
            save_project(context.project)
        except ProjectSaveException as e:
            self.report({'ERROR'}, "Failed to save project: {}".format(e))
            return {'CANCELLED'}

        return {'FINISHED'}


# -----------------------------------------------------------------------------
# Auto-loading / clearing of projects when loading/saving blend files.

@bpy.app.handlers.persistent
def on_blend_load(blend_path: str):
    if bpy.context.preferences.use_project_auto_save and bpy.context.project.is_dirty and bpy.context.project.data is not None:
        save_project(bpy.context.project)

    load_project_for_blend_path(bpy.context, blend_path)


@bpy.app.handlers.persistent
def on_blend_save(blend_path: str):
    # Auto-save on blend save.
    if bpy.context.preferences.use_project_auto_save and bpy.context.project.is_dirty and bpy.context.project.data is not None:
        save_project(bpy.context.project)

    # This is needed so that when saving a new file to a directory
    # in a project, the project is loaded.
    if bpy.context.project.data is None:
        load_project_for_blend_path(bpy.context, blend_path)


def on_exit():
    # TODO: when this gets called on exit, the assignment to project.is_dirty in
    # `save_project()` above appears to be the cause of a write-after-free bug.
    if bpy.context.preferences.use_project_auto_save and bpy.context.project.is_dirty and bpy.context.project.data is not None:
        save_project(bpy.context.project)


# -----------------------------------------------------------------------------
# Register

classes = (
    PROJECT_OP_NewProject,
    PROJECT_OP_SaveProject
)


def register():
    bpy.app.handlers.load_pre.append(on_blend_load)
    bpy.app.handlers.save_post.append(on_blend_save)

    atexit.register(on_exit)


def unregister():
    bpy.app.handlers.load_pre.remove(on_blend_load)
    bpy.app.handlers.save_post.append(on_blend_save)

    # Note: we intentionally *don't* call `atexit.unregister()`, because then
    # the callback gets removed before Python exits and thus doesn't run,
    # defeating the purpose.
