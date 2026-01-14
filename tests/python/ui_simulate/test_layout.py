# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything, its methods are accessed for tests by ``run_blender_setup.py``.
"""


def _string_search_property_cb(self, context, edit_text):
    return ["A", "C"]


class _StaticTestData:
    panel_draw_once = False


def _test_property_group_class():
    from bpy.props import (
        BoolProperty,
        BoolVectorProperty,
        CollectionProperty,
        EnumProperty,
        FloatProperty,
        FloatVectorProperty,
        IntProperty,
        IntVectorProperty,
        StringProperty,
    )
    from bpy.types import (
        PropertyGroup,
        OperatorFileListElement,
    )

    class TestPropertyGroup(PropertyGroup):
        bool_prop: BoolProperty()
        bool_vector_prop: BoolVectorProperty()

        int_prop: IntProperty()
        int_vector_prop: IntVectorProperty()
        int_xyz_prop: IntVectorProperty(subtype='XYZ')
        # Although this is a valid declaration it crashes if is used in `UILayout.prop(...)`
        int_color_prop: IntVectorProperty(subtype='COLOR', size=4)

        float_prop: FloatProperty()
        float_vector_prop: FloatVectorProperty()
        float_xyz_prop: FloatVectorProperty(subtype='XYZ')
        float_color_prop: FloatVectorProperty(subtype='COLOR', size=4, min=0.0, max=1.0)

        string_prop: StringProperty()
        string_search_prop: StringProperty(search=_string_search_property_cb)

        enum_prop: EnumProperty(items=[('A', "A", "A", 'ICON_NONE', 1),
                                       ('B', "B", "B", 'ICON_NONE', 2),
                                       ('C', "C", "C", 'ICON_NONE', 4)])
        enum_flag_prop: EnumProperty(items=[('A', "A", "A", 'ICON_NONE', 1),
                                            ('B', "B", "B", 'ICON_NONE', 2),
                                            ('C', "C", "C", 'ICON_NONE', 4)],
                                     options={'ENUM_FLAG'})

        prop_search_filter: CollectionProperty(type=OperatorFileListElement)

        panel_expand: BoolProperty(default=True)
    return TestPropertyGroup


def _test_panel_class():
    from bpy.types import Panel

    class UI_PT_test(Panel):
        """Creates a Panel for testing UILayout methods"""
        bl_label = "Test"
        bl_idname = "UI_PT_test"
        bl_category = 'UI Test'
        bl_space_type = 'TEXT_EDITOR'
        bl_region_type = 'UI'

        def draw(self, context):
            data = context.scene.test_property_group

            layout = self.layout

            layout.separator(factor=1.5, type='LINE')
            layout.label(text="Bool Buttons")
            layout.prop(data, "bool_prop")
            layout.prop(data, "bool_vector_prop")

            # Int Properties
            layout.separator(factor=1.5, type='LINE')
            layout.label(text="Int Buttons")
            # Int Property button
            layout.prop(data, "int_prop")
            # Int Vector Property buttons for every entry
            layout.prop(data, "int_vector_prop")
            row = layout.row(align=True)
            col = row.column(align=True)
            # Int Vector Property buttons for each individual entry
            col.prop(data, "int_vector_prop", index=0)
            col.prop(data, "int_vector_prop", index=1)
            col.prop(data, "int_vector_prop", index=2)
            col = row.column(align=True)
            # Int Vector Property decorators for every entry
            col.prop_decorator(data, "int_vector_prop")
            col = row.column(align=True)
            # Int Vector Property decorators for each individual entry
            col.prop_decorator(data, "int_vector_prop", index=0)
            col.prop_decorator(data, "int_vector_prop", index=1)
            col.prop_decorator(data, "int_vector_prop", index=2)
            # Int Vector Property buttons with 'XYZ' sub type
            layout.prop(data, "int_xyz_prop")
            # WARNING: Int Vector Property with 'COLOR' as sub type crashes
            # layout.prop(data, "int_color_prop")

            # Float Properties
            layout.separator(factor=1.5, type='LINE')
            layout.label(text="Float Buttons")
            # Float Property Button
            layout.prop(data, "float_prop")
            # Float Vector Property buttons for every entry
            layout.prop(data, "float_vector_prop")
            row = layout.row(align=True)
            col = row.column(align=True)
            # Float Vector Property buttons for each individual entry
            col.prop(data, "float_vector_prop", index=0)
            col.prop(data, "float_vector_prop", index=1)
            col.prop(data, "float_vector_prop", index=2)
            col = row.column(align=True)
            # Float Vector Property decorators for every entry
            col.prop_decorator(data, "float_vector_prop")
            col = row.column(align=True)
            # Int Vector Property decorators for each individual entry
            col.prop_decorator(data, "float_vector_prop", index=0)
            col.prop_decorator(data, "float_vector_prop", index=1)
            col.prop_decorator(data, "float_vector_prop", index=2)
            # Float Vector Property buttons with 'XYZ' sub type
            layout.prop(data, "float_xyz_prop")
            # Float Vector Property buttons with 'XYZ' sub type
            layout.prop(data, "float_color_prop")

            # String Properties
            layout.separator(factor=1.5, type='LINE')
            layout.label(text="String Buttons")
            layout.prop(data, "string_prop")
            # String Properties with search callback
            layout.prop(data, "string_search_prop")
            layout.prop_search(data, "string_search_prop", data, "prop_search_filter")
            # String Properties with no search callback but with a collection filter
            layout.prop_search(data, "string_prop", data, "prop_search_filter")

            # Enum Properties
            layout.separator(factor=1.5, type='LINE')
            layout.label(text="Enum Buttons")
            # Enum Property as menu
            layout.prop(data, "enum_prop")
            # Enum Property as radio buttons
            layout.prop(data, "enum_prop", expand=True)
            # Enum Property as tabs, this looks little bit weird in vertical panels
            layout.column(align=True).prop_tabs_enum(data, "enum_prop")
            # Enum Property as radio buttons, similar to UILayout.prop(..., expand=True)
            layout.props_enum(data, "enum_prop")

            # Enum Property as radio menu
            layout.prop_menu_enum(data, "enum_prop")
            row = layout.row(align=True)
            # Enum Property button for each enum value
            row.prop_enum(data, "enum_prop", value='A')
            row.prop_enum(data, "enum_prop", value='B')
            row.prop_enum(data, "enum_prop", value='C')
            # Enum Property menu with search filter
            layout.prop_search(data, "enum_prop", data, "prop_search_filter")

            # Enum Flag Properties
            layout.separator(factor=1.5, type='LINE')
            layout.label(text="Enum Flag Buttons")
            # Enum Property buttons for every enum flag value
            layout.prop(data, "enum_flag_prop")
            row = layout.row(align=True)
            # Enum Property button for each individual enum flag value
            row.prop_enum(data, "enum_flag_prop", value='A')
            row.prop_enum(data, "enum_flag_prop", value='B')
            row.prop_enum(data, "enum_flag_prop", value='C')

            # Popovers
            layout.separator(factor=1.5, type='LINE')
            layout.label(text="Popover Buttons")
            layout.popover("UI_PT_test")

            # Layout Panel
            layout.separator(factor=1.5, type='LINE')
            layout.label(text="Layout Panel")
            header, body = layout.panel_prop(data, "panel_expand")
            header.label(text="Panel Header")
            # LAyout panel body should be expanded by default
            assert body
            body.label(text="Panel Body")

            _StaticTestData.panel_draw_once = True

    return UI_PT_test


def _register():
    import bpy
    classes = (_test_property_group_class(), _test_panel_class())
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


def ui_layout_buttons():
    _register()
    e, t = _test_vars(window := _test_window())

    area = window.screen.areas[0]
    area.type = 'TEXT_EDITOR'
    t.assertEqual(area.type, 'TEXT_EDITOR')

    area.spaces[0].show_region_ui = True
    t.assertEqual(area.spaces[0].show_region_ui, True)

    # Let UI to refresh so 'UI Test' can be set as ARegion::active_panel_category
    yield

    t.assertEqual(area.regions[2].type, 'UI')
    area.regions[2].active_panel_category = 'UI Test'
    t.assertEqual(area.regions[2].active_panel_category, 'UI Test')
    yield

    t.assertEqual(_StaticTestData.panel_draw_once, True)
    yield
