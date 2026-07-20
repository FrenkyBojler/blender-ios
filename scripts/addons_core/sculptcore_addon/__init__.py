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

from . import convert, cursor, engine, engine_props, gestures, handlers, keymap, menus, ops, props, stroke, tools, ui, undo, vanilla_panels


class SculptCoreMode(bpy.types.ObjectModeType):
    bl_idname = "sculptcore.sculpt"
    bl_label = "SculptCore"
    bl_icon = 'SCULPTMODE_HLT'
    bl_object_types = {'MESH'}
    bl_keymap = "SculptCore Mode"
    bl_default_tool = "sculptcore.brush"
    # The brush-asset shelf that polls this mode (ui.py registers it); the
    # header popup selector resolves it via
    # #BrushAssetShelf.get_shelf_name_from_context.
    bl_brush_asset_shelf = "SCULPTCORE_AST_brush_sculpt"
    # Tier-2 delta undo: each stroke pushes a CUSTOM_MODE step wrapping a
    # meshlog step id (see undo.py). The Mesh ID still stays authoritative
    # through flush for save/render; memfile remains the boundary fallback.
    bl_use_custom_undo = True
    # The mode's tools use the shared sculpt brush, so paint-context lookups
    # (brush texture user in the texture properties tab etc.) resolve to it.
    bl_use_sculpt_paint = True

    def enter(self, context, ob):
        convert.enter(ob)

    def exit(self, context, ob):
        convert.exit_(ob)

    def flush(self, ob):
        convert.flush(ob)

    def refresh(self, context, ob):
        convert.refresh(ob)

    def undo_decode(self, context, ob, state_id, direction, is_final):
        undo.decode(context, ob, state_id, direction, is_final)

    def undo_free(self, state_id):
        undo.free(state_id)

    def draw_cursor(self, context, x, y):
        cursor.draw(context, x, y)


def register():
    props.register()
    engine_props.register()
    stroke.register()
    ops.register()
    gestures.register()
    # Hand the mode the native external draw provider so custom-mode objects
    # draw their per-node geometry from the engine (P5 D6). Best-effort: if the
    # engine is unavailable the mode still registers and falls back to the
    # flush-to-Mesh draw path.
    try:
        SculptCoreMode.bl_draw_provider = str(int(engine.capi().lib.sc_external_draw_provider()))
    except Exception as ex:
        print("SculptCore: external draw provider unavailable ({!r}); "
              "using the flush-to-Mesh draw path".format(ex))
    bpy.utils.register_class(SculptCoreMode)
    keymap.register()
    tools.register()
    # The vanilla brush-panel subclasses first: ui.py parents its engine
    # panel under SCULPTCORE_PT_tools_brush_settings.
    vanilla_panels.register()
    ui.register()
    menus.register()
    handlers.register()


def unregister():
    # Unregistering the mode type force-exits every object still in the
    # mode (exit -> flush -> free) before the class goes away; this only
    # catches sessions those exits left behind.
    handlers.unregister()
    menus.unregister()
    ui.unregister()
    vanilla_panels.unregister()
    tools.unregister()
    keymap.unregister()
    bpy.utils.unregister_class(SculptCoreMode)
    gestures.unregister()
    ops.unregister()
    stroke.unregister()
    engine_props.unregister()
    props.unregister()
    engine.free_all_sessions()
    undo.reset()
