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


def asset_index_main(args: list[str]) -> int:
    """Run the `blender -c asset_index` CLI command.

    This is late-importing the cli module, so that it (and its
    dependencies) are only imported when actually used.
    """
    import traceback
    from . import cli

    try:
        cli.main(args)
    except SystemExit as ex:
        if isinstance(ex.code, int):
            return ex.code
        return 2
    except BaseException:
        traceback.print_exc()
        return 1
    return 0


def register() -> None:
    handle = bpy.utils.register_cli_command("asset_index", asset_index_main)
    _cli_command_handles.append(handle)


def unregister() -> None:
    for handle in _cli_command_handles:
        bpy.utils.unregister_cli_command(handle)
    _cli_command_handles.clear()
