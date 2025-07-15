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

import bpy

@bpy.app.handlers.persistent
def dummy(filepath: str):
    if filepath == "":
        print("Not an on-disk blend file.")
        return

    filepath = Path(filepath)
    for parent in filepath.parents:
        project_config_path = parent.joinpath(".blender_project")
        if project_config_path.is_dir():
            print("Found project root: ", parent)
            bpy.context.project.init("Foo", str(parent))
            return

    print("Didn't find any project root.")
    bpy.context.project.clear()



####################
# REGISTER

def register():
    bpy.app.handlers.load_pre.append(dummy)

def unregister():
    bpy.app.handlers.load_pre.remove(dummy)
