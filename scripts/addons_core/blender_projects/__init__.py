# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

bl_info = {
    "name": "Blender Projects",
    "version": (0, 1, 0),
    "author": "Nathan Vegdahl",
    "blender": (5, 0, 0),
    "description": "TODO",
    "location": "TODO",
    "doc_url": "{BLENDER_MANUAL_URL}/addons/TODO",
    "support": "OFFICIAL",
    "category": "TODO",
}

from pathlib import Path

import tomllib

import bpy


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
    if root_path == None:
        return

    config = read_project_config(root_path)
    if config == None:
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
