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


def read_project(root_path: Path) -> str | None:
    name = "My Project"

    config_path = root_path.joinpath(PROJECT_DIR, PROJECT_CONFIG)
    try:
        with open(config_path, "rb") as f:
            data = tomllib.load(f)
    except FileNotFoundError:
        return name

    if "name" in data and type(data["name"]) is str:
        name = data["name"]

    return name


@bpy.app.handlers.persistent
def dummy(filepath: str) -> None:
    if filepath == "":
        print("Not an on-disk blend file.")
        return

    filepath = Path(filepath)
    for parent in filepath.parents:
        if parent.joinpath(PROJECT_DIR).is_dir():
            print("Found project root: ", parent)
            project_name = read_project(parent)
            bpy.context.project.init(project_name, str(parent))
            return

    print("Didn't find any project root.")
    bpy.context.project.clear()


####################
# REGISTER

def register():
    bpy.app.handlers.load_pre.append(dummy)

def unregister():
    bpy.app.handlers.load_pre.remove(dummy)
