# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

bl_info = {
    "name": "Remote Assets",
    "author": "Sybren Stüvel",
    "version": (0, 0, 1),
    "blender": (4, 4, 0),
    "location": "Nobody Knows",
    "description": "Glue Add-on for remote asset libraries",
    "warning": "",
    # "doc_url": "",
    "support": 'OFFICIAL',
    "category": "System",
}


def register():
    import sys

    do_reload = 'bl_assets' in sys.modules

    from _bpy_internal.assets import remote_library_index

    if do_reload:
        import importlib

        remote_library_index = importlib.reload(remote_library_index)

    remote_library_index.register()


def unregister():
    from _bpy_internal.assets import remote_library_index

    remote_library_index.unregister()
