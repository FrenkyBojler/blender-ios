# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything, it's methods are accessed for tests by: ``run.py``.
"""


def _test_window(windows_exclude=None):
    import bpy
    wm = bpy.data.window_managers[0]
    # Use -1 so the last added window is always used.
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


def _call_by_name(e, text: str):
    yield e.f3()
    yield e.text(text)
    yield e.ret()


def check_all_workspaces():
    """Test that we can cycle through all default workspaces without crashing"""
    e, t = _test_vars(window := _test_window())

    t.assertEqual(window.workspace.name_full, "Layout")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "Modeling")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "Sculpting")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "UV Editing")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "Texture Paint")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "Shading")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "Animation")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "Rendering")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "Compositing")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "Geometry Nodes")

    yield from _call_by_name(e, "Next Workspace")
    t.assertEqual(window.workspace.name_full, "Scripting")
