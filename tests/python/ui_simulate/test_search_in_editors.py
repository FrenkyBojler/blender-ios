"""
This file does not run anything; its methods are accessed by run_blender_setup.py.

Tests for search/filter functionality in:
  - Properties editor
  - Outliner
  - Dope Sheet
  - Graph Editor
  - File Browser

Requires: tests/files/ui_tests/test_search_in_editors.blend
  Objects expected in that file:
    __search_test_cube__  — mesh with a Subdivision Surface modifier
    __anim_test_obj__     — mesh with location keyframes on frames 1 and 10
"""

import os
import modules.ui_test_utils as ui

_BLEND_FILE = os.path.join(
    os.path.dirname(__file__),
    "..", "..", "files", "ui_tests", "test_search_in_editors.blend",
)


def _set_area_type(area, area_type):
    """Switch *area* to *area_type*."""
    import bpy
    with bpy.context.temp_override(area=area):
        area.type = area_type
    yield


def _get_action_fcurves(action):
    """Return F-Curves for an Action, supporting both Legacy and Slotted formats."""
    if getattr(action, "is_action_legacy", True):
        return list(getattr(action, "fcurves", []))
    fcurves = []
    for layer in getattr(action, "layers", []):
        for strip in getattr(layer, "strips", []):
            for bag in getattr(strip, "channelbags", []):
                fcurves.extend(getattr(bag, "fcurves", []))
    return fcurves


def _load_blend(t):
    """Load the test blend file; re-acquire and return (e, t, window, area)."""
    import bpy
    t.assertTrue(os.path.isfile(_BLEND_FILE), f"Test blend file not found: {_BLEND_FILE}")
    bpy.ops.wm.open_mainfile(filepath=_BLEND_FILE)
    yield  # wait for file load to complete
    e, t, window = ui.test_window()
    area = ui.largest_area(window.screen)
    return e, t, window, area


def test_properties_search():
    """
    Properties editor — Ctrl+F → 'subdivision'.
    Verifies the search filter matches the Subdivision Surface modifier on
    __search_test_cube__ and that clearing the field resets it.
    """
    import bpy

    _, t, _ = ui.test_window()
    e, t, window, area = yield from _load_blend(t)

    yield from _set_area_type(area, 'PROPERTIES')

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceProperties, "Area did not switch to Properties editor")
    t.assertIn("__search_test_cube__", bpy.data.objects, "Blend file is missing __search_test_cube__")

    # Activate the object so its modifier panels are visible.
    bpy.context.view_layer.objects.active = bpy.data.objects["__search_test_cube__"]
    yield

    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.ctrl.f()
    yield e.text("subdivision")
    yield e.ret()
    t.assertEqual(space.search_filter, "subdivision", "Properties: search_filter was not set by Ctrl+F")

    yield e.ctrl.f()
    yield e.back_space()
    yield e.ret()
    t.assertEqual(space.search_filter, "", "Properties: search_filter should be empty after clearing")


def test_outliner_search():
    """
    Outliner — Ctrl+F → '__search_test_cube__'.
    Verifies filter_text is set and cleared correctly.
    """
    import bpy

    _, t, _ = ui.test_window()
    e, t, window, area = yield from _load_blend(t)

    yield from _set_area_type(area, 'OUTLINER')

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceOutliner, "Area did not switch to Outliner")
    t.assertIn("__search_test_cube__", bpy.data.objects, "Blend file is missing __search_test_cube__")

    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.ctrl.f()
    yield e.text("__search_test_cube__")
    yield e.ret()
    t.assertEqual(space.filter_text, "__search_test_cube__", "Outliner: filter_text was not set by Ctrl+F")

    yield e.ctrl.f()
    yield e.back_space()
    yield e.ret()
    t.assertEqual(space.filter_text, "", "Outliner: filter_text was not cleared")


def test_dopesheet_search():
    """
    Dope Sheet — Ctrl+F → 'location'.
    Requires __anim_test_obj__ with at least one location F-Curve in the blend file.
    """
    import bpy

    _, t, _ = ui.test_window()
    e, t, window, area = yield from _load_blend(t)

    yield from _set_area_type(area, 'DOPESHEET_EDITOR')
    with bpy.context.temp_override(area=area):
        area.spaces.active.ui_mode = 'DOPESHEET'
    yield  # wait for mode switch

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceDopeSheetEditor, "Area did not switch to Dope Sheet")

    t.assertIn("__anim_test_obj__", bpy.data.objects, "Blend file is missing __anim_test_obj__")
    obj = bpy.data.objects["__anim_test_obj__"]
    t.assertIsNotNone(obj.animation_data,
                      "__anim_test_obj__ has no animation_data — insert location keyframes and resave")
    t.assertIsNotNone(obj.animation_data.action,
                      "__anim_test_obj__ has no action — insert location keyframes and resave")
    location_curves = [fc for fc in _get_action_fcurves(obj.animation_data.action)
                       if fc.data_path == "location"]
    t.assertGreater(len(location_curves), 0,
                    "__anim_test_obj__ has no location F-Curves — insert keyframes and resave")

    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.ctrl.f()
    yield e.text("location")
    yield e.ret()
    t.assertEqual(space.dopesheet.filter_fcurve_name, "location",
                  "Dope Sheet: filter_fcurve_name was not set by Ctrl+F")

    yield e.ctrl.f()
    yield e.back_space()
    yield e.ret()
    t.assertEqual(space.dopesheet.filter_fcurve_name, "",
                  "Dope Sheet: filter_fcurve_name was not cleared")


def test_graph_editor_search():
    """
    Graph Editor — Ctrl+F → 'location'.
    Requires __anim_test_obj__ with at least one location F-Curve in the blend file.
    """
    import bpy

    _, t, _ = ui.test_window()
    e, t, window, area = yield from _load_blend(t)

    yield from _set_area_type(area, 'GRAPH_EDITOR')

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceGraphEditor, "Area did not switch to Graph Editor")

    t.assertIn("__anim_test_obj__", bpy.data.objects, "Blend file is missing __anim_test_obj__")
    obj = bpy.data.objects["__anim_test_obj__"]
    t.assertIsNotNone(obj.animation_data, "__anim_test_obj__ has no animation_data")
    t.assertIsNotNone(obj.animation_data.action, "__anim_test_obj__ has no action")
    location_curves = [fc for fc in _get_action_fcurves(obj.animation_data.action)
                       if fc.data_path == "location"]
    t.assertGreater(len(location_curves), 0, "__anim_test_obj__ has no location F-Curves")

    e.cursor_position_set(*ui.get_area_center(area), move=True)
    yield e.ctrl.f()
    yield e.text("location")
    yield e.ret()
    t.assertEqual(space.dopesheet.filter_fcurve_name, "location",
                  "Graph Editor: filter_fcurve_name was not set by Ctrl+F")

    yield e.ctrl.f()
    yield e.back_space()
    yield e.ret()
    t.assertEqual(space.dopesheet.filter_fcurve_name, "",
                  "Graph Editor: filter_fcurve_name was not cleared")


def test_file_browser_search():
    """
    File Browser — Ctrl+F → 'search_target'.
    """
    import bpy
    import tempfile

    _, t, _ = ui.test_window()
    e, t, window, area = yield from _load_blend(t)

    yield from _set_area_type(area, 'FILE_BROWSER')

    space = area.spaces.active
    t.assertIsInstance(space, bpy.types.SpaceFileBrowser, "Area did not switch to File Browser")

    with tempfile.TemporaryDirectory() as tmpdir:
        open(os.path.join(tmpdir, "search_target_file.blend"), 'w').close()

        with bpy.context.temp_override(area=area):
            bpy.ops.file.select_bookmark(dir=tmpdir)
        yield  # wait for navigation to complete

        params = space.params
        t.assertIsNotNone(params, "File Browser: params is None after navigation")

        e.cursor_position_set(*ui.get_area_center(area), move=True)
        yield e.ctrl.f()
        yield e.text("search_target")
        yield e.ret()
        t.assertEqual(params.filter_search, "search_target",
                      "File Browser: filter_search was not set by Ctrl+F")

        yield e.ctrl.f()
        yield e.back_space()
        yield e.ret()
        t.assertEqual(params.filter_search, "", "File Browser: filter_search was not cleared")
