# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for opening all editor types in Blender.
Covers the area listed in UI Test To-Dos #151680.
This file does not run anything, its methods are accessed
for tests by run_blender_setup.py.
"""

import modules.ui_test_utils as ui

EDITOR_TYPES = {
    'VIEW_3D': 1,
    'IMAGE_EDITOR': 1,
    'NODE_EDITOR': 1,
    'SEQUENCE_EDITOR': 1,
    'CLIP_EDITOR': 1,
    'DOPESHEET_EDITOR': 1,
    'GRAPH_EDITOR': 1,
    'NLA_EDITOR': 1,
    'TEXT_EDITOR': 1,
    'CONSOLE': 1,
    'INFO': 1,
    'OUTLINER': 1,
    'PROPERTIES': 1,
    'FILE_BROWSER': 1,
    'SPREADSHEET': 1,
}


def test_open_editor_types():
    """
    Test that all editor types can be opened without error
    and that each editor draws at least one region correctly.
    Addresses: UI Test To-Dos #151680 - Opening all editor types.
    """
    import bpy
    e, t, window = ui.test_window()

    area = window.screen.areas[0]
    original_type = area.type

    for editor_type, min_regions in EDITOR_TYPES.items():

        with bpy.context.temp_override(area=area):
            area.type = editor_type
            yield

            t.assertEqual(
                area.type,
                editor_type,
                msg="Editor failed to switch to: {}".format(editor_type)
            )

            t.assertGreaterEqual(
                len(area.regions),
                min_regions,
                msg="Editor {} has no regions, may have failed to initialize".format(editor_type)
            )

    area.type = original_type
    yield
