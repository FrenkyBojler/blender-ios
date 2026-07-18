# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The "SculptCore Mode" keymap (referenced by SculptCoreMode.bl_keymap; the
viewport's dynamic keymap handler activates it while the mode is active).

LMB strokes, Ctrl-LMB inverts, Shift-LMB smooths; F / Shift-F run the
standard radial controls on the shared sculpt Paint's size/strength
(unified-aware, same property paths as the vanilla sculpt keymap).
"""

import bpy

_KEYMAP_NAME = "SculptCore Mode"

_BRUSH_PATH = "tool_settings.sculpt.brush"
_UNIFIED_PATH = "tool_settings.sculpt.unified_paint_settings"


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
    _radial(km, "size", "use_unified_size")
    _radial(km, "strength", "use_unified_strength", shift=True)


def unregister():
    wm = bpy.context.window_manager
    kc = wm.keyconfigs.addon
    if kc is None:
        return
    km = kc.keymaps.get(_KEYMAP_NAME)
    if km is not None:
        kc.keymaps.remove(km)
