# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
SculptCore sculpt mode — a first-class object mode implemented as an addon
on the custom-mode infrastructure (bpy.types.ObjectModeType).

v0 slice: enter/exit with positions-only conversion; the Mesh ID stays
authoritative through the mode's flush callback (memfile undo + save work
unchanged). Stroke operator, draw provider and wrapped undo land on top.
"""

bl_info = {
    "name": "SculptCore Sculpt Mode",
    "author": "Blender Authors",
    "version": (0, 1, 0),
    "blender": (5, 3, 0),
    "location": "3D Viewport > Mode dropdown",
    "description": "Sculpt mode built on the SculptCore engine",
    "category": "Sculpting",
}

import bpy

from . import convert, engine, handlers, keymap, props, stroke, tools, ui


class SculptCoreMode(bpy.types.ObjectModeType):
    bl_idname = "sculptcore.sculpt"
    bl_label = "SculptCore"
    bl_icon = 'SCULPTMODE_HLT'
    bl_object_types = {'MESH'}
    bl_keymap = "SculptCore Mode"
    bl_default_tool = "sculptcore.brush"
    # Inert until the wrapped undo type lands (undo-integration plan);
    # Tier-1 sessions ride memfile undo through flush/refresh.
    bl_use_custom_undo = False

    def enter(self, context, ob):
        convert.enter(ob)

    def exit(self, context, ob):
        convert.exit_(ob)

    def flush(self, ob):
        convert.flush(ob)

    def refresh(self, context, ob):
        convert.refresh(ob)


def register():
    props.register()
    stroke.register()
    bpy.utils.register_class(SculptCoreMode)
    keymap.register()
    tools.register()
    ui.register()
    handlers.register()


def unregister():
    # Unregistering the mode type force-exits every object still in the
    # mode (exit -> flush -> free) before the class goes away; this only
    # catches sessions those exits left behind.
    handlers.unregister()
    ui.unregister()
    tools.unregister()
    keymap.unregister()
    bpy.utils.unregister_class(SculptCoreMode)
    stroke.unregister()
    props.unregister()
    engine.free_all_sessions()
