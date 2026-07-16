# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The "SculptCore Mode" keymap (referenced by SculptCoreMode.bl_keymap; the
viewport's dynamic keymap handler activates it while the mode is active).

v0: LMB starts a stroke; Ctrl-LMB inverts (read as event.ctrl in the stroke
operator). Broader bindings (smooth on Shift, radius/strength radials) land
with the usability pass.
"""

import bpy

_KEYMAP_NAME = "SculptCore Mode"


def register():
    wm = bpy.context.window_manager
    kc = wm.keyconfigs.addon
    if kc is None:
        return
    km = kc.keymaps.new(name=_KEYMAP_NAME, space_type='EMPTY', region_type='WINDOW')
    km.keymap_items.new("sculptcore.brush_stroke", 'LEFTMOUSE', 'PRESS')
    km.keymap_items.new("sculptcore.brush_stroke", 'LEFTMOUSE', 'PRESS', ctrl=True)


def unregister():
    wm = bpy.context.window_manager
    kc = wm.keyconfigs.addon
    if kc is None:
        return
    km = kc.keymaps.get(_KEYMAP_NAME)
    if km is not None:
        kc.keymaps.remove(km)
