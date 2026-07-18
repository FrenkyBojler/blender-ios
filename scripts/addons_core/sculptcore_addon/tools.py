# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Toolbar tool for the mode.

Custom modes share the ``CTX_MODE_CUSTOM`` tool-storage slot (both the C
tool system and the Python toolbar key off it), so the tool registers under
the ``'CUSTOM'`` context-mode key. The stroke itself comes from the
"SculptCore Mode" keymap (via the viewport's dynamic keymap handler), so the
tool carries no keymap of its own — it is the toolbar presence + brush
cursor that makes the mode's default tool resolve (silencing the
`builtin.select_box not found` fallback).
"""

import bpy

_CONTEXT_MODE = 'CUSTOM'


class SculptCoreBrushTool(bpy.types.WorkSpaceTool):
    bl_space_type = 'VIEW_3D'
    bl_context_mode = _CONTEXT_MODE
    bl_idname = "sculptcore.brush"
    bl_label = "Brush"
    bl_description = "Sculpt with the active brush"
    bl_icon = "ops.sculpt.border_hide"
    bl_widget = None
    # Stroke input is handled by the "SculptCore Mode" keymap.
    bl_keymap = None

    def draw_settings(context, layout, _tool):
        sculpt = context.tool_settings.sculpt
        brush = sculpt.brush
        if brush is None:
            return
        # Route size/strength to the unified settings when they own the value
        # (what the stroke and cursor read) — a slider bound to the brush's
        # own field would be inert then.
        unified = sculpt.unified_paint_settings
        layout.prop(brush, "sculpt_brush_type", text="")
        layout.prop(unified if unified.use_unified_size else brush,
                    "size", text="Size", slider=True)
        layout.prop(unified if unified.use_unified_strength else brush,
                    "strength", text="Strength")


def register():
    from bl_ui.space_toolsystem_toolbar import VIEW3D_PT_tools_active

    # register_tool indexes _tools[context_mode] directly, so the custom-mode
    # slot must exist first (built-in modes have static entries).
    VIEW3D_PT_tools_active._tools.setdefault(_CONTEXT_MODE, [None])
    bpy.utils.register_tool(SculptCoreBrushTool, separator=False)


def unregister():
    try:
        bpy.utils.unregister_tool(SculptCoreBrushTool)
    except Exception:
        pass
