# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Menubar menus for the mode (P10 Phase C), mirroring vanilla sculpt's
View3D menus entry-for-entry where the engine supports the operator
(audit-gated: unsupported entries are omitted, recorded in
ui-parity-allowlist.json). Menus register only once they have content —
the Sculpt and Face Sets menus land with their engine operators.
"""

import bpy

_MODE = "sculptcore.sculpt"


def _in_mode(context):
    ob = context.active_object
    return ob is not None and ob.mode == 'CUSTOM' and ob.custom_mode == _MODE


def _flood_fill(layout, text, mode, value=0.0):
    props = layout.operator("sculptcore.mask_flood_fill", text=text)
    props.mode = mode
    props.value = value
    return props


def _mask_filter(layout, text, filter_type, auto=True):
    props = layout.operator("sculptcore.mask_filter", text=text)
    props.filter_type = filter_type
    props.auto_iteration_count = auto
    return props


class SCULPTCORE_MT_mask(bpy.types.Menu):
    bl_idname = "SCULPTCORE_MT_mask"
    bl_label = "Mask"

    def draw(self, _context):
        layout = self.layout
        # Same labels/order as vanilla VIEW3D_MT_mask (unsupported entries
        # omitted per the audit allowlist).
        _flood_fill(layout, "Fill Mask", 'VALUE', 1.0)
        _flood_fill(layout, "Clear Mask", 'VALUE', 0.0)
        _flood_fill(layout, "Invert Mask", 'INVERT')
        layout.separator()
        props = layout.operator("sculptcore.mask_box_gesture", text="Box Mask")
        props.mode = 'VALUE'
        props.value = 1.0
        props = layout.operator("sculptcore.mask_lasso_gesture", text="Lasso Mask")
        props.mode = 'VALUE'
        props.value = 1.0
        layout.separator()
        _mask_filter(layout, "Smooth Mask", 'SMOOTH')
        _mask_filter(layout, "Sharpen Mask", 'SHARPEN')
        _mask_filter(layout, "Grow Mask", 'GROW')
        _mask_filter(layout, "Shrink Mask", 'SHRINK')
        layout.separator()
        _mask_filter(layout, "Increase Contrast", 'CONTRAST_INCREASE', auto=False)
        _mask_filter(layout, "Decrease Contrast", 'CONTRAST_DECREASE', auto=False)


class SCULPTCORE_MT_mask_edit_pie(bpy.types.Menu):
    bl_idname = "SCULPTCORE_MT_mask_edit_pie"
    bl_label = "Mask Edit"

    def draw(self, _context):
        # Same slots as vanilla VIEW3D_MT_sculpt_mask_edit_pie.
        pie = self.layout.menu_pie()
        _flood_fill(pie, "Invert Mask", 'INVERT')
        _flood_fill(pie, "Clear Mask", 'VALUE', 0.0)
        _mask_filter(pie, "Smooth Mask", 'SMOOTH')
        _mask_filter(pie, "Sharpen Mask", 'SHARPEN')
        _mask_filter(pie, "Grow Mask", 'GROW')
        _mask_filter(pie, "Shrink Mask", 'SHRINK')
        _mask_filter(pie, "Increase Contrast", 'CONTRAST_INCREASE', auto=False)
        _mask_filter(pie, "Decrease Contrast", 'CONTRAST_DECREASE', auto=False)


class SCULPTCORE_MT_sculpt(bpy.types.Menu):
    bl_idname = "SCULPTCORE_MT_sculpt"
    bl_label = "Sculpt"

    def draw(self, context):
        layout = self.layout
        # Vanilla's "Dynamic Topology Toggle"; the engine's dyntopo is a
        # scene property, so a prop toggle replaces the operator.
        layout.prop(context.scene, "sculptcore_dyntopo", text="Dynamic Topology")


class SCULPTCORE_MT_face_sets(bpy.types.Menu):
    bl_idname = "SCULPTCORE_MT_face_sets"
    bl_label = "Face Sets"

    def draw(self, _context):
        layout = self.layout
        props = layout.operator("sculptcore.face_sets_create", text="Face Set from Masked")
        props.mode = 'MASKED'
        layout.separator()
        props = layout.operator("sculptcore.face_set_edit", text="Grow Face Set")
        props.mode = 'GROW'
        props = layout.operator("sculptcore.face_set_edit", text="Shrink Face Set")
        props.mode = 'SHRINK'


def _editor_menus(self, context):
    if _in_mode(context):
        # Vanilla menubar order: Sculpt, Mask, Face Sets.
        self.layout.menu(SCULPTCORE_MT_sculpt.bl_idname)
        self.layout.menu(SCULPTCORE_MT_mask.bl_idname)
        self.layout.menu(SCULPTCORE_MT_face_sets.bl_idname)


_classes = (
    SCULPTCORE_MT_sculpt,
    SCULPTCORE_MT_mask,
    SCULPTCORE_MT_mask_edit_pie,
    SCULPTCORE_MT_face_sets,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.VIEW3D_MT_editor_menus.append(_editor_menus)


def unregister():
    bpy.types.VIEW3D_MT_editor_menus.remove(_editor_menus)
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
