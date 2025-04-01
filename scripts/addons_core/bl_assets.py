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

    import bl_assets

    if do_reload:
        import importlib

        bl_assets = importlib.reload(bl_assets)

    bl_assets.register()


def unregister():
    import bl_assets

    bl_assets.unregister()
