# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything, it's methods are accessed for tests by: ``run.py``.
"""


def _test_window(windows_exclude=None):
    import bpy
    wm = bpy.data.window_managers[0]
    if windows_exclude is None:
        return wm.windows[0]
    for window in wm.windows:
        if window not in windows_exclude:
            return window
    return None


def _test_vars(window):
    import unittest
    from modules.easy_keys import EventGenerate
    return (
        EventGenerate(window),
        unittest.TestCase(),
    )


def _window_area_get_by_type(window, space_type):
    for area in window.screen.areas:
        if area.type == space_type:
            return area


def sculpt_mode_toolbar():
    e, t = _test_vars(window := _test_window())

    # In the default properties area, set it to the tool tab to force access of all
    # tool properties when a tool is activated.
    properties_area = _window_area_get_by_type(window, 'PROPERTIES')
    properties_area.spaces[0].context = 'TOOL'

    yield e.ctrl.tab().s()              # Sculpt via pie menu.

    tools = [
        "builtin.brush",
        "builtin_brush.paint",
        "builtin_brush.mask",
        "builtin_brush.draw_face_sets",
        "builtin.box_mask",
        "builtin.lasso_mask",
        "builtin.line_mask",
        "builtin.polyline_mask",
        "builtin.box_hide",
        "builtin.lasso_hide",
        "builtin.line_hide",
        "builtin.polyline_hide",
        "builtin.box_face_set",
        "builtin.lasso_face_set",
        "builtin.line_face_set",
        "builtin.polyline_face_set",
        "builtin.box_trim",
        "builtin.lasso_trim",
        "builtin.line_trim",
        "builtin.polyline_trim",
        "builtin.line_project",
        "builtin.mesh_filter",
        "builtin.cloth_filter",
        "builtin.color_filter",
        "builtin.face_set_edit",
        "builtin.mask_by_color",
        "builtin.move",
        "builtin.rotate",
        "builtin.scale",
        "builtin.transform",
        "builtin.annotate",
    ]

    import bpy
    for tool in tools:
        bpy.ops.wm.tool_set_by_id(name=tool, space_type='VIEW_3D')
        t.assertEqual(window.workspace.tools.from_space_view3d_mode('SCULPT').idname, tool)
