# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path
import tomllib

import bpy
from bpy.types import Operator


# Directory and file name where the project is read/written to disk.
PROJECT_DIR = ".blender_project"
PROJECT_CONFIG = "project.toml"


# -------------------------------------------------------------
# Custom exception types, for anticipated errors that should be reported to the
# user.

class ProjectSaveException(Exception):
    pass


class ProjectLoadException(Exception):
    pass


# -------------------------------------------------------------

def escape_string(text):
    """ Escape a string according the required escapes in
        https://toml.io/en/v1.1.0#string
    """

    # First replace literal backslashes.
    text = text.replace("\\", "\\\\")

    # Then the rest.
    required_escapes = [
        # Quotes.
        "\"",
        # U+0000 to U+0008.
        "\x00", "\x01", "\x02", "\x03", "\x04", "\x05" "\x06", "\x07", "\x08",
        # U+000A to U+001F.
        "\x0A", "\x0B", "\x0C", "\x0D", "\x0E", "\x0F", "\x10", "\x11", "\x12",
        "\x13", "\x14", "\x15", "\x16", "\x17", "\x18", "\x19", "\x1A", "\x1B",
        "\x1C", "\x1D", "\x1E", "\x1F",
        # U+007F.
        "\x7F",
    ]
    for esc in required_escapes:
        text = text.replace(esc, f"\\{esc}")

    return text


def save_project(project):
    """ Saves the passed project to disk.

        Throws a ProjectSaveException in any of the following cases:

        - There is no project to save.
        - The project's root path is relative or doesn't exist.
        - The project can't be written due to any of a number of filesystem
          issues (directory isn't writable, etc.).
    """

    if project.data is None:
        raise ProjectSaveException("Cannot save project because there is no project to save.")

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
            f.write("name = \"{}\"\n".format(escape_string(data.name)))
    except PermissionError:
        raise ProjectSaveException("Cannot write to '{}' due to filesystem permissions.".format(PROJECT_CONFIG))

    project.is_dirty = False

    print("...done.")


def find_and_load_project_for_blend_path(context, blend_path):
    """ Finds and loads the project that the specified blend file belongs to, or
        clears the project if no project is found.

        Throws a ProjectLoadException if a project is found but is invalid
        (missing config file, config validation error, etc.).
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


def find_project_root_from_blend_file_path(blend_path):
    """ Searches for a Blender project root in the parent directories of the
        given path.

        Returns the project root if found, or None otherwise.
    """

    for parent in blend_path.parents:
        if parent.joinpath(PROJECT_DIR).is_dir():
            return parent
    return None


def read_project_toml_config(root_path):
    """ Reads the project config for the given project root path.

        Throws a ProjectLoadException if no config is found, if the config is
        not readable due to filesystem permissions, or if it contains invalid
        TOML.

        Returns the config as a Python dictionary.
    """
    config_path = root_path.joinpath(PROJECT_DIR, PROJECT_CONFIG)
    try:
        with open(config_path, "rb") as f:
            return tomllib.load(f)
    except FileNotFoundError:
        raise ProjectLoadException("Project has no {} file.".format(PROJECT_CONFIG))
    except PermissionError:
        raise ProjectLoadException("Cannot access {} file due to filesystem permissions.".format(PROJECT_CONFIG))
    except tomllib.TOMLDecodeError as e:
        raise ProjectLoadException("Project's {} file contains invalid TOML.".format(PROJECT_CONFIG))


def validate_config(config_dict):
    """ Checks that the passed config is valid.

        This consists of ensuring that all required fields exist, and
        that all fields present are of the right type and have valid values.

        Throws a ProjectLoadException if there's a validation error.

        No return value.
    """
    if "name" not in config_dict:
        raise ProjectLoadException("Invalid project: no project name defined in '{}'.".format(PROJECT_CONFIG))
        return

    if type(config_dict["name"]) != str:
        raise ProjectLoadException("Invalid project: project name is not a string.")
        return

    if config_dict["name"] == "":
        raise ProjectLoadException("Invalid project: project name is empty.")
        return


def blend_file_is_in_valid_project(blend_file_path):
    """ Returns true if the specified blend file is inside a valid project, false if there is no project or it's invalid.

        An "invalid project" is one whose TOML config is non-existent or doesn't
        validate. See `validate_config()`.
    """
    project_root = find_project_root_from_blend_file_path(blend_file_path)
    if project_root is None:
        return False

    try:
        config_dict = read_project_toml_config(project_root)
        validate_config(config_dict)
    except ProjectLoadException:
        # No valid project found.
        return False

    return True


# -------------------------------------------------------------

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
        return context.project.data is None and bpy.data.filepath != ""

    def execute(self, context):
        if not bpy.context.preferences.experimental.use_blender_projects:
            self.report({'ERROR'}, "Blender Projects experimental feature not enabled.")
            return {'CANCELLED'}

        if self.directory == "":
            self.report({'ERROR'}, "Cannot create a project with an empty directory path.")
            return {'CANCELLED'}

        if not bpy.path.is_subdir(path=bpy.data.filepath, directory=self.directory):
            self.report({'ERROR'}, "New project directory must be a parent of the currently open blend file.")
            return {'CANCELLED'}

        # Double-check that we're not already in a project directory.
        #
        # Under normal circumstances this should never happen, because a project
        # would already be loaded in that case, and thus `poll()` would fail.
        # But if someone manually calls `context.project.clear()` then this can
        # happen.
        if blend_file_is_in_valid_project(Path(bpy.data.filepath)):
            self.report(
                {'ERROR'},
                "New project directory is already inside of an existing project. Try reloading the current blend file to open the existing project.")
            return {'CANCELLED'}

        # Get the initial project name based on the folder name.
        project_name = os.path.basename(os.path.normpath(self.directory)).title()

        # Create the project.
        context.project.init(project_name, self.directory)

        # Immediately save the project.
        try:
            save_project(context.project)
        except ProjectSaveException as e:
            self.report({'ERROR'}, "Failed to save project: {}".format(e))
            return {'CANCELLED'}

        return {'FINISHED'}

    def invoke(self, context, event):
        # Set our initial path as the directory that contains the currently open
        # blend file.
        dirpath = os.path.dirname(bpy.data.filepath)
        if dirpath != "":
            self.directory = dirpath

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
        if not bpy.context.preferences.experimental.use_blender_projects:
            self.report({'ERROR'}, "Blender Projects experimental feature not enabled.")
            return {'CANCELLED'}

        try:
            save_project(context.project)
        except ProjectSaveException as e:
            self.report({'ERROR'}, "Failed to save project: {}".format(e))
            return {'CANCELLED'}

        return {'FINISHED'}


class PROJECT_OP_OpenBlendInProject(Operator):
    """Opens a blend file, but only if it's inside of a project."""
    bl_idname = "project.open_blend_in_project"
    bl_label = "Open File..."

    filepath: bpy.props.StringProperty(
        name="Blend file path",
        subtype='FILE_PATH',
        default="",
    )

    filter_folder: bpy.props.BoolProperty(
        name="Filter folders",
        default=True,
        options={'HIDDEN'},
    )

    filter_blender: bpy.props.BoolProperty(
        name="Filter blend files",
        default=True,
        options={'HIDDEN'},
    )

    @classmethod
    def poll(cls, context):
        return True

    def execute(self, context):
        if not bpy.context.preferences.experimental.use_blender_projects:
            self.report({'ERROR'}, "Blender Projects experimental feature not enabled.")
            return {'CANCELLED'}

        if not blend_file_is_in_valid_project(Path(self.filepath)):
            self.report(
                {'ERROR'},
                "Selected blend file is not part of a project.")
            return {'CANCELLED'}

        bpy.ops.wm.open_mainfile(filepath=self.filepath)

        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}


# -------------------------------------------------------------
# Auto-loading / clearing of projects when loading/saving blend files or
# exiting.

@bpy.app.handlers.persistent
def on_blend_load(blend_path):
    if not bpy.context.preferences.experimental.use_blender_projects:
        return

    # Auto-save the current project before loading a different blend file.
    if bpy.context.preferences.use_project_auto_save and bpy.context.project.is_dirty and bpy.context.project.data is not None:
        save_project(bpy.context.project)

    # Load the project (or clear if none) for the blend file we're about to
    # load.
    find_and_load_project_for_blend_path(bpy.context, blend_path)


@bpy.app.handlers.persistent
def on_blend_save(blend_path):
    if not bpy.context.preferences.experimental.use_blender_projects:
        return

    # Auto-save project when saving the current blend file.
    if bpy.context.preferences.use_project_auto_save and bpy.context.project.is_dirty and bpy.context.project.data is not None:
        save_project(bpy.context.project)

    # In case we're saving the blend to disk for the first time or to a new
    # location, load the project there (if any).
    find_and_load_project_for_blend_path(bpy.context, blend_path)


@bpy.app.handlers.persistent
def on_exit(is_user_exit):
    if not is_user_exit:
        return

    if not bpy.context.preferences.experimental.use_blender_projects:
        return

    if bpy.context.preferences.use_project_auto_save and bpy.context.project.is_dirty and bpy.context.project.data is not None:
        save_project(bpy.context.project)


# -----------------------------------------------------------------------------
# Register

classes = (
    PROJECT_OP_NewProject,
    PROJECT_OP_SaveProject,
    PROJECT_OP_OpenBlendInProject,
)


def register():
    bpy.app.handlers.load_pre.append(on_blend_load)
    bpy.app.handlers.save_post.append(on_blend_save)
    bpy.app.handlers.exit_pre.append(on_exit)


def unregister():
    bpy.app.handlers.load_pre.remove(on_blend_load)
    bpy.app.handlers.save_post.remove(on_blend_save)
    bpy.app.handlers.exit_pre.remove(on_exit)
