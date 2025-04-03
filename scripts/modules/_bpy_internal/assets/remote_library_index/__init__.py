# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ------------------------------------------------------------------------
# NOTE: BIG FAT UGLYNESS WARNING
#
# This package has `register()` and `unregister()` functions, but Blender
# doesn't find those without help. So I made a tiny little core addon that
# calls these functions.
#
# This should be temporary.
# ------------------------------------------------------------------------

# The `blender_asset_library_openapi.py` file is generated from the
# corresponding YAML file, which contains an OpenAPI service specification.
#
# Run `make generate_datamodels` from Blender's top source directory to
# regenerate the Python code based on the YAML contents.


import bpy

_cli_command_handles = []


def main(args: list[str]) -> int:
    """Run the asset_index command.

    This is late-importing the __main__ module, so that it (and its
    dependencies) are imported when actually used.
    """
    import traceback
    from . import __main__

    try:
        __main__.main(args)
    except Exception as ex:
        traceback.print_exc()
        return 1
    return 0


def register() -> None:
    handle = bpy.utils.register_cli_command("asset_index", main)
    _cli_command_handles.append(handle)


def unregister() -> None:
    for handle in _cli_command_handles:
        bpy.utils.unregister_cli_command(handle)
    _cli_command_handles.clear()
