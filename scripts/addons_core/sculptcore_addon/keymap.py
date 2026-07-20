# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The "SculptCore Mode" keymap (referenced by SculptCoreMode.bl_keymap; the
viewport's dynamic keymap handler activates it while the mode is active).

Vanilla-chord-compatible (tracked by the T3 keymap diff): LMB strokes,
Ctrl-LMB inverts, Shift-LMB smooths; F / Shift-F radial size/strength
(unified-aware, same property paths as vanilla); Alt-M / Ctrl-I mask
clear/invert; RMB / menu key context menu; Ctrl-digit / Alt-1/2 multires
levels.
"""

import bpy

_KEYMAP_NAME = "SculptCore Mode"

_BRUSH_PATH = "tool_settings.sculpt.brush"
_UNIFIED_PATH = "tool_settings.sculpt.unified_paint_settings"

# Essentials brush-asset shortcuts, verbatim from the vanilla Sculpt keymap
# (T3). Brushes are shared assets, so activation works in-mode; brushes
# whose type has no engine kernel activate but refuse to stroke.
_ESSENTIALS = "brushes/essentials_brushes-mesh_sculpt.blend/Brush/"
_BRUSH_ASSET_KEYS = (
    ('V', {}, "Draw", False),
    ('S', {}, "Smooth", False),
    ('P', {}, "Pinch/Magnify", False),
    ('I', {}, "Inflate/Deflate", False),
    ('G', {}, "Grab", False),
    ('T', {"shift": True}, "Scrape/Fill", False),
    ('C', {}, "Clay Strips", False),
    ('C', {"shift": True}, "Crease Polish", False),
    ('K', {}, "Snake Hook", False),
    ('M', {}, "Mask", True),
)


def _radial(km, prop, unified_prop, **kwargs):
    kmi = km.keymap_items.new("wm.radial_control", 'F', 'PRESS', **kwargs)
    props = kmi.properties
    props.data_path_primary = "{:s}.{:s}".format(_BRUSH_PATH, prop)
    props.data_path_secondary = "{:s}.{:s}".format(_UNIFIED_PATH, prop)
    props.use_secondary = "{:s}.{:s}".format(_UNIFIED_PATH, unified_prop)
    props.rotation_path = "{:s}.texture_slot.angle".format(_BRUSH_PATH)
    props.color_path = "{:s}.cursor_color_add".format(_BRUSH_PATH)
    props.image_id = _BRUSH_PATH


def register():
    wm = bpy.context.window_manager
    kc = wm.keyconfigs.addon
    if kc is None:
        return
    km = kc.keymaps.new(name=_KEYMAP_NAME, space_type='EMPTY', region_type='WINDOW')
    km.keymap_items.new("sculptcore.brush_stroke", 'LEFTMOUSE', 'PRESS')
    kmi = km.keymap_items.new("sculptcore.brush_stroke", 'LEFTMOUSE', 'PRESS', ctrl=True)
    kmi.properties.mode = 'INVERT'
    kmi = km.keymap_items.new("sculptcore.brush_stroke", 'LEFTMOUSE', 'PRESS', shift=True)
    kmi.properties.mode = 'SMOOTH'
    # Alt-stroke masks; Ctrl inverts it live (the operator reads event.ctrl).
    kmi = km.keymap_items.new("sculptcore.brush_stroke", 'LEFTMOUSE', 'PRESS', alt=True)
    kmi.properties.mode = 'MASK'
    kmi = km.keymap_items.new("sculptcore.brush_stroke", 'LEFTMOUSE', 'PRESS', ctrl=True, alt=True)
    kmi.properties.mode = 'MASK'
    _radial(km, "size", "use_unified_size")
    _radial(km, "strength", "use_unified_strength", shift=True)

    # Brush selection + sizing + stroke toggles (all shared-brush state).
    for key, mods, brush_name, use_toggle in _BRUSH_ASSET_KEYS:
        kmi = km.keymap_items.new("brush.asset_activate", key, 'PRESS', **mods)
        kmi.properties.asset_library_type = 'ESSENTIALS'
        kmi.properties.relative_asset_identifier = _ESSENTIALS + brush_name
        if use_toggle:
            kmi.properties.use_toggle = True
    kmi = km.keymap_items.new("brush.scale_size", 'LEFT_BRACKET', 'PRESS')
    kmi.properties.scalar = 0.9
    kmi = km.keymap_items.new("brush.scale_size", 'RIGHT_BRACKET', 'PRESS')
    kmi.properties.scalar = 1.0 / 0.9
    kmi = km.keymap_items.new("wm.context_toggle", 'S', 'PRESS', shift=True)
    kmi.properties.data_path = "{:s}.use_smooth_stroke".format(_BRUSH_PATH)
    kmi = km.keymap_items.new("wm.context_menu_enum", 'E', 'PRESS', alt=True)
    kmi.properties.data_path = "{:s}.stroke_method".format(_BRUSH_PATH)
    kmi = km.keymap_items.new("wm.call_asset_shelf_popover", 'SPACE', 'PRESS', shift=True)
    kmi.properties.name = "SCULPTCORE_AST_brush_sculpt"

    # Texture-angle radial (vanilla: Ctrl-F) + stencil placement + color flip.
    kmi = km.keymap_items.new("wm.radial_control", 'F', 'PRESS', ctrl=True)
    props = kmi.properties
    props.data_path_primary = "{:s}.texture_slot.angle".format(_BRUSH_PATH)
    props.rotation_path = "{:s}.texture_slot.angle".format(_BRUSH_PATH)
    props.color_path = "{:s}.cursor_color_add".format(_BRUSH_PATH)
    props.image_id = _BRUSH_PATH
    for mods, mode, texmode in (
            ({}, 'TRANSLATION', None),
            ({"shift": True}, 'SCALE', None),
            ({"ctrl": True}, 'ROTATION', None),
            ({"alt": True}, 'TRANSLATION', 'SECONDARY'),
            ({"shift": True, "alt": True}, 'SCALE', 'SECONDARY'),
            ({"ctrl": True, "alt": True}, 'ROTATION', 'SECONDARY'),
    ):
        kmi = km.keymap_items.new("brush.stencil_control", 'RIGHTMOUSE', 'PRESS', **mods)
        kmi.properties.mode = mode
        if texmode is not None:
            kmi.properties.texmode = texmode
    km.keymap_items.new("paint.brush_colors_flip", 'X', 'PRESS')

    # Mask edit pie (vanilla: A).
    kmi = km.keymap_items.new("wm.call_menu_pie", 'A', 'PRESS')
    kmi.properties.name = "SCULPTCORE_MT_mask_edit_pie"

    # Mask edits (vanilla chords: Alt-M clear, Ctrl-I invert).
    kmi = km.keymap_items.new("sculptcore.mask_flood_fill", 'M', 'PRESS', alt=True)
    kmi.properties.mode = 'VALUE'
    kmi.properties.value = 0.0
    kmi = km.keymap_items.new("sculptcore.mask_flood_fill", 'I', 'PRESS', ctrl=True)
    kmi.properties.mode = 'INVERT'

    # Mask gestures (vanilla chords: B box with value 0, Ctrl-RMB lasso
    # clear, Ctrl-Shift-RMB lasso fill).
    kmi = km.keymap_items.new("sculptcore.mask_box_gesture", 'B', 'PRESS')
    kmi.properties.mode = 'VALUE'
    kmi.properties.value = 0.0
    kmi = km.keymap_items.new("sculptcore.mask_lasso_gesture", 'RIGHTMOUSE', 'PRESS', ctrl=True)
    kmi.properties.value = 0.0
    kmi = km.keymap_items.new(
        "sculptcore.mask_lasso_gesture", 'RIGHTMOUSE', 'PRESS', ctrl=True, shift=True)
    kmi.properties.value = 1.0

    # Context menu (vanilla: RMB and the menu key call the sculpt panel).
    for key in ('RIGHTMOUSE', 'APP'):
        kmi = km.keymap_items.new("wm.call_panel", key, 'PRESS')
        kmi.properties.name = "SCULPTCORE_PT_sculpt_context_menu"

    # Face set grow/shrink under the cursor (vanilla: Ctrl-W / Ctrl-Alt-W).
    kmi = km.keymap_items.new("sculptcore.face_set_edit", 'W', 'PRESS', ctrl=True)
    kmi.properties.mode = 'GROW'
    kmi = km.keymap_items.new("sculptcore.face_set_edit", 'W', 'PRESS', ctrl=True, alt=True)
    kmi.properties.mode = 'SHRINK'

    # Dyntopo detail size (vanilla: R): radial-edit the active detail prop.
    km.keymap_items.new("sculptcore.dyntopo_detail_size_edit", 'R', 'PRESS')

    # Multires levels (vanilla: Ctrl-digit absolute, Alt-1/2 relative).
    for index, key in enumerate(('ZERO', 'ONE', 'TWO', 'THREE', 'FOUR', 'FIVE')):
        kmi = km.keymap_items.new("sculptcore.subdivision_set", key, 'PRESS', ctrl=True)
        kmi.properties.level = index
        kmi.properties.relative = False
    for key, delta in (('ONE', -1), ('TWO', 1)):
        kmi = km.keymap_items.new("sculptcore.subdivision_set", key, 'PRESS', alt=True)
        kmi.properties.level = delta
        kmi.properties.relative = True


def unregister():
    wm = bpy.context.window_manager
    kc = wm.keyconfigs.addon
    if kc is None:
        return
    km = kc.keymaps.get(_KEYMAP_NAME)
    if km is not None:
        kc.keymaps.remove(km)
