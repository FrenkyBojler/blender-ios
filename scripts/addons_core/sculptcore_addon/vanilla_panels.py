# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Vanilla brush-panel reuse (P10 Phase B).

The brush state is the shared ``tool_settings.sculpt``, so the vanilla brush
panels' draw code is already correct for this mode — what hides them is
bl_context gating and the mode dispatch in UnifiedPaintPanel.get_brush_mode
(which maps CUSTOM to 'SCULPT' for modes declaring bl_use_sculpt_paint).
Each clone below reuses draw() unchanged and overrides only the
registration surface: idname, category/context, parent chain, and a poll
gated on this mode.

Clone, not subclass: registering a subclass of a *registered* class makes
`bpy.types.<BaseName>` return a bare RNA wrapper without the base's Python
methods (verified against stock Blender 5.0, not specific to this branch),
which would break any later `bpy.types.VIEW3D_PT_tools_brush_*` use. So the
factory rebuilds each class from the vanilla class's own dict on its
(unregistered) mixin bases. The vanilla classes use no super() calls, so
copied methods bind cleanly.

Panels registered this way appear in the sidebar Tool tab and (top-level
ones) as tool-header popovers via the custom-mode popover_group.
"""

import bpy

_MODE_CONTEXT = "sculptcore.sculpt"

# (bl_ui module, vanilla class name, our class name, our parent or None).
# Order registers parents before children.
_SPECS = (
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_select",
     "SCULPTCORE_PT_tools_brush_select", None),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_settings",
     "SCULPTCORE_PT_tools_brush_settings", None),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_settings_advanced",
     "SCULPTCORE_PT_tools_brush_settings_advanced", "SCULPTCORE_PT_tools_brush_settings"),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_stroke",
     "SCULPTCORE_PT_tools_brush_stroke", "SCULPTCORE_PT_tools_brush_settings"),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_stroke_smooth_stroke",
     "SCULPTCORE_PT_tools_brush_stroke_smooth_stroke", "SCULPTCORE_PT_tools_brush_stroke"),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_falloff",
     "SCULPTCORE_PT_tools_brush_falloff", "SCULPTCORE_PT_tools_brush_settings"),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_falloff_normal",
     "SCULPTCORE_PT_tools_brush_falloff_normal", "SCULPTCORE_PT_tools_brush_falloff"),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_display",
     "SCULPTCORE_PT_tools_brush_display", "SCULPTCORE_PT_tools_brush_settings"),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_texture",
     "SCULPTCORE_PT_tools_brush_texture", "SCULPTCORE_PT_tools_brush_settings"),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_color",
     "SCULPTCORE_PT_tools_brush_color", "SCULPTCORE_PT_tools_brush_settings"),
    ("space_view3d_toolbar", "VIEW3D_PT_tools_brush_swatches",
     "SCULPTCORE_PT_tools_brush_swatches", "SCULPTCORE_PT_tools_brush_settings"),
    # The RMB context menu (popover-only panel; the keymap's wm.call_panel
    # opens it by name).
    ("space_view3d", "VIEW3D_PT_sculpt_context_menu",
     "SCULPTCORE_PT_sculpt_context_menu", None),
)

def _poll(cls, context):
    ob = context.active_object
    return (
        ob is not None
        and ob.mode == 'CUSTOM'
        and ob.custom_mode == _MODE_CONTEXT
        and context.tool_settings.sculpt.brush is not None
    )


def _draw_texture(self, context):
    # Vanilla VIEW3D_PT_tools_brush_texture.draw, except the sculpt flag is
    # hardcoded True: vanilla derives it from context.sculpt_object, which
    # only exists in the built-in sculpt mode.
    layout = self.layout
    from bl_ui.properties_paint_common import brush_texture_settings

    settings = self.paint_settings_from_active_tool(context)
    brush = settings.brush
    tex_slot = brush.texture_slot

    col = layout.column()
    col.template_ID_preview(tex_slot, "texture", new="texture.new", rows=3, cols=8)

    brush_texture_settings(col, brush, True)


def _poll_color(cls, context):
    """Color panels only for color-capable brushes (engine COLOR kernel)."""
    return (_poll(cls, context)
            and context.tool_settings.sculpt.brush.sculpt_capabilities.has_color)


# Per-clone attribute overrides (applied after the vanilla dict copy and the
# default poll, so an entry here wins).
_OVERRIDES = {
    "SCULPTCORE_PT_tools_brush_texture": {"draw": _draw_texture},
    "SCULPTCORE_PT_tools_brush_color": {"poll": classmethod(_poll_color)},
    "SCULPTCORE_PT_tools_brush_swatches": {"poll": classmethod(_poll_color)},
}


_classes = []


def register():
    import importlib

    for module_name, vanilla_name, our_name, parent in _SPECS:
        module = importlib.import_module("bl_ui." + module_name)
        base = getattr(module, vanilla_name)
        attrs = {
            key: value for key, value in base.__dict__.items()
            if not key.startswith("__") and key != "bl_rna"
        }
        if getattr(base, "bl_region_type", 'WINDOW') == 'UI':
            # Sidebar panels; popover-only WINDOW panels (context menu)
            # reject categories.
            attrs["bl_category"] = "Tool"
            attrs["bl_context"] = _MODE_CONTEXT
        attrs["poll"] = classmethod(_poll)
        attrs.pop("bl_parent_id", None)
        if parent is not None:
            attrs["bl_parent_id"] = parent
        attrs.update(_OVERRIDES.get(our_name, ()))
        cls = type(our_name, base.__bases__, attrs)
        bpy.utils.register_class(cls)
        _classes.append(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
    _classes.clear()
