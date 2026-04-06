# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path
import logging

from attrs import define

import bpy
from bpy.types import Operator
from bpy.app.translations import pgettext_rpt as rpt_

logger = logging.getLogger(__name__)

# Directory and file name where the project is read/written to disk.
PROJECT_DIR = ".blender_project"
PROJECT_CONFIG = "project.toml"


# -------------------------------------------------------------
# Types that define the schema for reading/writing project config TOML files.

@define
class ProjectConfig:
    name: str


# -------------------------------------------------------------
# Custom exception types, for anticipated errors that should be reported to the
# user.

class ProjectSaveException(Exception):
    pass


class ProjectLoadException(Exception):
    pass


# -------------------------------------------------------------

def escape_string_toml(text):
    """Escape a string according TOML 1.1 spec.

    See https://toml.io/en/v1.1.0#string
    """

    # First replace literal backslashes.
    text = text.replace("\\", "\\\\")

    # Then the rest.
    required_escapes = [
        # Quotes.
        "\"",
        # U+0000 to U+0008.
        "\x00", "\x01", "\x02", "\x03", "\x04", "\x05", "\x06", "\x07", "\x08",
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


def save_project(project, report=None):
    """Save the passed project to disk.

    Throws a ProjectSaveException in any of the following cases:

    - There is no project to save.
    - The project's root path is relative or doesn't exist.
    - The project can't be written due to any of a number of filesystem
      issues (directory isn't writable, etc.).

    Optionally takes an `Operator.report` for reporting errors to the user.
    """

    if project is None:
        if report:
            report({'ERROR'}, "Cannot save project because there is no project to save.")
        raise ProjectSaveException

    logger.info("Saving project '{}' at '{}'...".format(project.name, project.root_path))

    root_path = Path(project.root_path)

    try:
        if not root_path.is_absolute():
            if report:
                report({'ERROR'}, "Cannot write project to non-absolute path.")
            raise ProjectSaveException

        if not root_path.is_dir():
            if report:
                report({'ERROR'}, "Cannot save project: root directory does not exist.")
            raise ProjectSaveException
    except PermissionError:
        if report:
            report({'ERROR'}, rpt_("Cannot access '{}' due to filesystem permissions.").format(PROJECT_DIR))
        raise ProjectSaveException

    config_dir_path = root_path.joinpath(PROJECT_DIR)

    try:
        config_dir_path.mkdir(parents=True, exist_ok=True)
    except FileExistsError:
        if report:
            report({'ERROR'}, rpt_("A file named '{}' already exists, but it needs to be a directory.").format(PROJECT_DIR))
        raise ProjectSaveException
    except PermissionError:
        if report:
            report({'ERROR'}, "Cannot create '{}' directory due to filesystem permissions.".format(PROJECT_DIR))
        raise ProjectSaveException

    config_path = root_path.joinpath(PROJECT_DIR, PROJECT_CONFIG)
    try:
        with config_path.open(mode='w', encoding='utf-8') as f:
            # The actual project file writing.
            f.write("name = \"{}\"\n".format(escape_string_toml(project.name)))
    except PermissionError:
        if report:
            report({'ERROR'}, rpt_("Cannot write to '{}' due to filesystem permissions.").format(PROJECT_CONFIG))
        raise ProjectSaveException

    project.is_dirty = False

    logger.info("...done.")


def find_and_load_project_for_blend_path(context, blend_path, report=None):
    """Load the project the blend file is in, or clears the project if none is found.

    Throws a ProjectLoadException if a project is found but is invalid
    (missing config file, config validation error, etc.).

    Optionally takes an `Operator.report` for reporting errors to the user.
    """

    if blend_path == "":
        # Not an on-disk blend file, so there is no project to load.
        bpy.data.project_clear()
        return

    root_path = find_project_root_from_blend_file_path(Path(blend_path))
    if root_path is None:
        # No project.
        bpy.data.project_clear()
        return

    if bpy.data.project is not None and root_path == bpy.data.project.root_path:
        # We already have this project loaded, and we don't want to obliterate
        # local unsaved changes if auto-save isn't turned on.
        return

    bpy.data.project_clear()

    # Load project.
    config = read_project_toml_config(root_path, report)
    bpy.data.project_init(config.name, str(root_path))
    bpy.data.project.is_dirty = False


def find_project_root_from_blend_file_path(blend_path):
    """Search for a project root in the parent directories of the given path.

    Returns the project root if found, or None otherwise.
    """

    for parent in blend_path.parents:
        if parent.joinpath(PROJECT_DIR).is_dir():
            return parent
    return None


def read_project_toml_config(root_path, report=None) -> ProjectConfig:
    """Read the project config for the given project root path.

    Throws a ProjectLoadException if no config is found, if the config is
    not readable due to filesystem permissions, or if it's not a valid
    project config (e.g. contains invalid TOML or doesn't match the schema).

    Optionally takes an `Operator.report` for reporting errors to the user.

    Returns the configuration (`ProjectConfig`).
    """
    import tomllib
    import cattrs

    config_path = root_path.joinpath(PROJECT_DIR, PROJECT_CONFIG)
    try:
        with open(config_path, "rb") as f:
            config_dict = tomllib.load(f)
    except FileNotFoundError:
        if report:
            report({'ERROR'}, rpt_("Project has no {} file.").format(PROJECT_CONFIG))
        raise ProjectLoadException
    except PermissionError:
        if report:
            report({'ERROR'}, rpt_("Cannot access {} file due to filesystem permissions.").format(PROJECT_CONFIG))
        raise ProjectLoadException
    except tomllib.TOMLDecodeError as e:
        if report:
            report({'ERROR'}, rpt_("Project's {} file contains invalid TOML.").format(PROJECT_CONFIG))
        raise ProjectLoadException

    # Validate schema and convert to ProjectConfig class.
    converter = cattrs.Converter()
    project_config = converter.structure(config_dict, ProjectConfig)

    # Other validation not handled by the schema.
    if project_config.name == "":
        if report:
            report({'ERROR'}, "Invalid project: project name is empty.")
        raise ProjectLoadException

    return project_config


def blend_file_is_in_valid_project(blend_file_path):
    """Return whether the blend file is inside a valid project or not.

    True if the blend file is inside a valid project, false if no project is
    found or if the project is invalid.

    An "invalid project" is one whose TOML config is non-existent or doesn't
    validate. See `read_project_toml_config()`.
    """
    project_root = find_project_root_from_blend_file_path(blend_file_path)
    if project_root is None:
        return False

    try:
        _ = read_project_toml_config(project_root, None)
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
        return bpy.data.project is None and bpy.data.filepath != ""

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
        # But if someone manually calls `bpy.data.project_clear()` then this can
        # happen.
        if blend_file_is_in_valid_project(Path(bpy.data.filepath)):
            self.report(
                {'ERROR'},
                "New project directory is already inside of an existing project. Try reloading the current blend file to open the existing project.")
            return {'CANCELLED'}

        # Get the initial project name based on the folder name.
        project_name = os.path.basename(os.path.normpath(self.directory)).title()

        # Create the project.
        bpy.data.project_init(project_name, self.directory)

        # Immediately save the project.
        try:
            save_project(bpy.data.project, self.report)
        except ProjectSaveException as e:

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
        return bpy.data.project is not None

    def execute(self, context):
        if not bpy.context.preferences.experimental.use_blender_projects:
            self.report({'ERROR'}, "Blender Projects experimental feature not enabled.")
            return {'CANCELLED'}

        try:
            save_project(bpy.data.project)
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

def log_project_save_error():
    logger.error(f"Error trying to save project '{bpy.data.project.name}' at '{bpy.data.project.root_path}'.")


def log_project_load_error(blend_path):
    logger.error(f"Error trying to load project for blend file '{blend_path}'.")


@bpy.app.handlers.persistent
def on_blend_load(blend_path):
    if not bpy.context.preferences.experimental.use_blender_projects:
        return

    # Auto-save the current project before loading a different blend file.
    if bpy.context.preferences.use_project_auto_save and bpy.data.project is not None and bpy.data.project.is_dirty:
        try:
            save_project(bpy.data.project)
        except ProjectSaveException:
            log_project_save_error()

    # Load the project (or clear if none) for the blend file we're about to
    # load.
    try:
        find_and_load_project_for_blend_path(bpy.context, blend_path)
    except ProjectLoadException:
        log_project_load_error(blend_path)


@bpy.app.handlers.persistent
def on_blend_save(blend_path):
    if not bpy.context.preferences.experimental.use_blender_projects:
        return

    # Auto-save project when saving the current blend file.
    if bpy.context.preferences.use_project_auto_save and bpy.data.project is not None and bpy.data.project.is_dirty:
        try:
            save_project(bpy.data.project)
        except ProjectSaveException:
            log_project_save_error()

    # In case we're saving the blend to disk for the first time or to a new
    # location, load the project there (if any).
    try:
        find_and_load_project_for_blend_path(bpy.context, blend_path)
    except ProjectLoadException:
        log_project_load_error(blend_path)


@bpy.app.handlers.persistent
def on_exit(is_user_exit):
    if not is_user_exit:
        return

    if not bpy.context.preferences.experimental.use_blender_projects:
        return

    if bpy.context.preferences.use_project_auto_save and bpy.data.project is not None and bpy.data.project.is_dirty:
        try:
            save_project(bpy.data.project)
        except ProjectSaveException:
            log_project_save_error()


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
