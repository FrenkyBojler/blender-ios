# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path
import logging
from enum import Enum

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

class VariableType(Enum):
    INTEGER = 'INTEGER'
    FLOAT = 'FLOAT'
    STRING = 'STRING'
    FILEPATH = 'FILEPATH'


@define
class ProjectVariable:
    name: str
    type: VariableType
    value: int | str | float
    description: str | None = None


@define
class ProjectConfig:
    name: str
    variables: list[ProjectVariable] | None = None


def structure_int_float_str(obj: int | float | str, cl: type) -> int | float | str:
    if isinstance(obj, int) or isinstance(obj, float) or isinstance(obj, str):
        return obj
    else:
        raise ValueError(f"Cannot structure {obj!r} as int | float | str")


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

    logger.info("Saving project '{:s}' at '{:s}'...".format(project.name, project.root_path))

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
            report({'ERROR'}, rpt_("Cannot access '{:s}' due to filesystem permissions.").format(PROJECT_DIR))
        raise ProjectSaveException
    except Exception as e:
        if report:
            report({'ERROR'}, str(e))
        raise ProjectSaveException

    config_dir_path = root_path.joinpath(PROJECT_DIR)

    try:
        config_dir_path.mkdir(parents=True, exist_ok=True)
    except FileExistsError:
        if report:
            report({'ERROR'}, rpt_("A file named '{:s}' already exists, but it needs to be a directory.").format(PROJECT_DIR))
        raise ProjectSaveException
    except PermissionError:
        if report:
            report({'ERROR'}, rpt_("Cannot create '{:s}' directory due to filesystem permissions.").format(PROJECT_DIR))
        raise ProjectSaveException
    except Exception as e:
        if report:
            report({'ERROR'}, str(e))
        raise ProjectSaveException

    config_path = root_path.joinpath(PROJECT_DIR, PROJECT_CONFIG)
    try:
        with config_path.open(mode='w', encoding='utf-8') as f:
            # The actual project file writing.
            f.write("name = \"{:s}\"\n\n".format(escape_string_toml(project.name)))

            for var in project.variables:
                f.write("[[variables]]\n")
                f.write("name = \"{:s}\"\n".format(escape_string_toml(var.name)))
                if var.description != "":
                    f.write("description = \"{:s}\"\n".format(escape_string_toml(var.description)))
                f.write("type = \"{:s}\"\n".format(var.type))
                match var.type:
                    case 'INTEGER':
                        f.write("value = {:d}\n".format(var.value_int))
                    case 'FLOAT':
                        f.write("value = {:f}\n".format(var.value_float))
                    case 'STRING':
                        f.write("value = \"{:s}\"\n".format(escape_string_toml(var.value_string)))
                    case 'FILEPATH':
                        f.write("value = \"{:s}\"\n".format(escape_string_toml(var.value_string)))
                f.write("\n")
    except PermissionError:
        if report:
            report({'ERROR'}, rpt_("Cannot write to '{:s}' due to filesystem permissions.").format(PROJECT_CONFIG))
        raise ProjectSaveException
    except Exception as e:
        if report:
            report({'ERROR'}, str(e))
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

    if bpy.data.project is not None and os.path.normpath(root_path) == os.path.normpath(bpy.data.project.root_path):
        # We already have this project loaded, and we don't want to obliterate
        # local unsaved changes if auto-save isn't turned on.
        return

    bpy.data.project_clear()

    # Load project.
    config = read_project_toml_config(root_path, report)
    bpy.data.project_init(config.name, str(root_path))
    if config.variables is not None:
        for config_var in config.variables:
            var = bpy.data.project.variables.new()
            var.name = config_var.name
            var.type = config_var.type.value
            match config_var.type:
                case VariableType.INTEGER:
                    var.value_int = config_var.value
                case VariableType.FLOAT:
                    var.value_float = config_var.value
                case VariableType.STRING:
                    var.value_string = config_var.value
                case VariableType.FILEPATH:
                    var.value_string = config_var.value
            if config_var.description is not None:
                var.description = config_var.description

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
            report({'ERROR'}, rpt_("Project has no {:s} file.").format(PROJECT_CONFIG))
        raise ProjectLoadException
    except PermissionError:
        if report:
            report({'ERROR'}, rpt_("Cannot access {:s} file due to filesystem permissions.").format(PROJECT_CONFIG))
        raise ProjectLoadException
    except tomllib.TOMLDecodeError as e:
        if report:
            report({'ERROR'}, rpt_("Project's {:s} file contains invalid TOML: {:s}").format(PROJECT_CONFIG, str(e)))
        raise ProjectLoadException
    except Exception as e:
        if report:
            report({'ERROR'}, str(e))
        raise ProjectLoadException

    # Validate schema and convert to ProjectConfig class.
    converter = cattrs.Converter()
    converter.register_structure_hook(int | float | str, structure_int_float_str)
    try:
        project_config = converter.structure(config_dict, ProjectConfig)
    except cattrs.BaseValidationError as e:
        if report:
            report({'ERROR'}, rpt_("Invalid project configuration file: {:s}").format(str(e)))
        raise ProjectLoadException

    # Other validation not handled by the schema.
    if project_config.name == "":
        if report:
            report({'ERROR'}, "Invalid project: project name is empty.")
        raise ProjectLoadException

    # TODO: make sure variable values match the variable type.

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
        except ProjectSaveException:
            # Reporting is handled by `save_project()` call in the `try` block.
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
            save_project(bpy.data.project, self.report)
        except ProjectSaveException:
            # Reporting is handled by `save_project()` call in the `try` block.
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


class PROJECT_OP_AddVariable(Operator):
    """Add a new variable to the current project"""
    bl_idname = "project.add_variable"
    bl_label = "Add Variable"

    @classmethod
    def poll(cls, context):
        return bpy.data.project is not None

    def execute(self, context):
        var = bpy.data.project.variables.new()
        var.name = "Variable"

        return {'FINISHED'}


class PROJECT_OP_RemoveVariable(Operator):
    """Removes the active variable from the current project"""
    bl_idname = "project.remove_variable"
    bl_label = "Remove Variable"

    @classmethod
    def poll(cls, context):
        project = bpy.data.project
        if project is None:
            return False
        return project.active_variable_index < len(project.variables)

    def execute(self, context):
        project = bpy.data.project
        var = project.variables[project.active_variable_index]
        project.variables.remove(var)

        return {'FINISHED'}


class PROJECT_OP_MoveVariable(Operator):
    """Move the active variable up or down in the list of variables"""
    bl_idname = "project.move_variable"
    bl_label = "Move Variable"

    direction: bpy.props.EnumProperty(items=[
        ('UP', "Move Up", ""),
        ('DOWN', "Move Down", ""),
    ])

    @classmethod
    def poll(cls, context):
        project = bpy.data.project
        if project is None:
            return False
        return project.active_variable_index < len(project.variables)

    def execute(self, context):
        project = bpy.data.project

        index = project.active_variable_index
        if self.direction == 'UP' and index > 0:
            project.variables.move(index, index - 1)
            project.active_variable_index -= 1
        elif self.direction == 'DOWN' and (index + 1) < len(project.variables):
            project.variables.move(index, index + 1)
            project.active_variable_index += 1
        return {'FINISHED'}


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
    PROJECT_OP_AddVariable,
    PROJECT_OP_RemoveVariable,
    PROJECT_OP_MoveVariable,
)


def register():
    bpy.app.handlers.load_pre.append(on_blend_load)
    bpy.app.handlers.save_post.append(on_blend_save)
    bpy.app.handlers.exit_pre.append(on_exit)


def unregister():
    bpy.app.handlers.load_pre.remove(on_blend_load)
    bpy.app.handlers.save_post.remove(on_blend_save)
    bpy.app.handlers.exit_pre.remove(on_exit)
