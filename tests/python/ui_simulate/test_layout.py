# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything, its methods are accessed for tests by ``run_blender_setup.py``.
"""


def _string_search_property_cb(self, context, edit_text):
    return ["A", "C"]


def _test_string_prop_group_class():
    from bpy.props import (
        BoolProperty,
        StringProperty,
        CollectionProperty,
    )
    from bpy.types import (
        PropertyGroup,
        OperatorFileListElement,
    )

    class TestStringPropertyGroup(PropertyGroup):
        bool_prop: BoolProperty()

        string_prop: StringProperty()
        string_update_prop: StringProperty(options={'TEXTEDIT_UPDATE'})
        string_search_prop: StringProperty(search=_string_search_property_cb)
        string_force_search_value_prop: StringProperty(search_options={'SORT'})
        prop_search_filter: CollectionProperty(type=OperatorFileListElement)

    return TestStringPropertyGroup


def _test_string_prop_button_panel_class():
    from bpy.types import Panel

    class UI_PT_string_property_buttons(Panel):
        bl_label = "Test String Property Buttons"
        bl_idname = "UI_PT_string_prop_button_test"
        bl_category = 'Test String Property Buttons'
        bl_space_type = 'TEXT_EDITOR'
        bl_region_type = 'UI'

        def draw(self, context):
            data = context.scene.test_property_group
            layout = self.layout

            layout.prop(data, "string_prop")

            layout.prop(data, "string_update_prop")

            # String Properties with search callback
            layout.prop(data, "string_search_prop")
            layout.prop_search(data, "string_search_prop", data, "prop_search_filter")
            # String Properties with no search callback but with a collection filter
            layout.prop_search(data, "string_prop", data, "prop_search_filter")

            layout.prop(data, "string_force_search_value_prop")

    return UI_PT_string_property_buttons


def _register():
    import bpy
    classes = (_test_string_prop_group_class(), _test_string_prop_button_panel_class())
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.test_property_group = bpy.props.PointerProperty(type=classes[0])
    bpy.data.scenes['Scene'].test_property_group.prop_search_filter.add().name = "A"
    bpy.data.scenes['Scene'].test_property_group.prop_search_filter.add().name = "B"


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


def ui_string_property_buttons():
    _register()
    e, t = _test_vars(window := _test_window())

    area = window.screen.areas[0]
    area.type = 'TEXT_EDITOR'
    t.assertEqual(area.type, 'TEXT_EDITOR')

    area.spaces[0].show_region_ui = True
    t.assertTrue(area.spaces[0].show_region_ui)
    # Let UI to refresh so 'UI Test' can be set as ARegion::active_panel_category
    yield

    region = area.regions[2]

    t.assertEqual(region.type, 'UI')
    area.regions[2].active_panel_category = 'Test String Property Buttons'
    t.assertEqual(region.active_panel_category, 'Test String Property Buttons')
    yield

    import bpy
    data = bpy.data.scenes['Scene'].test_property_group
    wm = bpy.data.window_managers[0]

    # Fail to open bool property as 'TEXT_EDITING'
    t.assertFalse(wm.try_activate_rna_button(region, data, "bool_prop", 'TEXT_EDITING'))

    # Open string_prop button as 'TEXT_EDITING' and type "123" as value
    t.assertTrue(wm.try_activate_rna_button(region, data, "string_prop", 'TEXT_EDITING'))
    yield e.text("123")
    t.assertEqual(data.string_prop, "")
    yield e.numpad_enter()
    t.assertEqual(data.string_prop, "123")

    # Open string_prop button as 'TEXT_EDITING', since button selects all text by default back_space clears the string
    t.assertTrue(wm.try_activate_rna_button(region, data, "string_prop", 'TEXT_EDITING'))
    yield e.back_space().ret()
    t.assertEqual(data.string_prop, "")

    # Type "123456789" as value
    t.assertTrue(wm.try_activate_rna_button(region, data, "string_prop", 'TEXT_EDITING'))
    yield e.text("123456789").ret()
    t.assertEqual(data.string_prop, "123456789")

    # Open string_prop button as 'TEXT_EDITING', move the cursor after last character to remove it
    t.assertTrue(wm.try_activate_rna_button(region, data, "string_prop", 'TEXT_EDITING'))
    yield e.right_arrow().back_space().ret()
    t.assertEqual(data.string_prop, "12345678")

    # Activate string_prop button as 'HIGHLIGHT', set it as 'TEXT_EDITING' with left click and type "a1"
    xy = wm.try_activate_rna_button(region, data, "string_prop", 'HIGHLIGHT')
    t.assertTrue(xy)
    yield e.cursor_position_set(*xy, move=False)
    yield e.leftmouse()
    yield e.text("a1").ret()
    t.assertEqual(data.string_prop, "a1")

    # Copy current "a1" and override it with "123"
    xy = wm.try_activate_rna_button(region, data, "string_prop", 'HIGHLIGHT')
    t.assertTrue(xy)
    yield e.cursor_position_set(*xy, move=False)
    yield e.ctrl.c()
    yield e.leftmouse()
    yield e.text("123").ret()

    t.assertEqual(data.string_prop, "123")

    # Paste previous "a1" value
    xy = wm.try_activate_rna_button(region, data, "string_prop", 'HIGHLIGHT')
    t.assertTrue(xy)
    yield e.cursor_position_set(*xy, move=False)
    yield e.ctrl.v()
    t.assertEqual(data.string_prop, "a1")
