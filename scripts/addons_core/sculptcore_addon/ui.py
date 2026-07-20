# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Sidebar Tool-tab UI for the mode (Brush/Automasking/Symmetry/Dyntopo/
Multires). Panels are gated by bl_context (the mode's idname, so they never
fight vanilla sculpt panels, whose context is ".sculpt_mode") and double as
tool-header popovers through the custom-mode popover_group. They read the
shared ``tool_settings.sculpt`` brush (brush-mapping decision 1). The
Multires panel exposes the modifier's ``sculpt_levels``, which the depsgraph
handler mirrors into the engine's active level (P8 C2).
"""

import bpy

from . import engine, engine_props, mapping, multires

# The standard category + the mode's context string: panels land in the
# sidebar "Tool" tab (where vanilla sculpt panels live) and are gated by
# bl_context matching, which for a custom mode is the registered idname
# (see view3d_sidebar_contexts / CTX_data_mode_string). The tool header's
# popover_group picks the same panels up as popovers.
_CATEGORY = "Tool"
_MODE_CONTEXT = "sculptcore.sculpt"


def _in_mode(context):
    ob = context.active_object
    return (
        ob is not None
        and ob.mode == 'CUSTOM'
        and ob.custom_mode == "sculptcore.sculpt"
    )


class SCULPTCORE_PT_brush_engine(bpy.types.Panel):
    """Engine-side brush state the vanilla panels don't cover: support
    warnings and the kernel's engine-only uniforms (generated group, M2).
    The standard brush UI (type/size/strength/stroke/falloff/texture) comes
    from the vanilla subclasses in vanilla_panels.py."""
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = _CATEGORY
    bl_context = _MODE_CONTEXT
    bl_parent_id = "SCULPTCORE_PT_tools_brush_settings"
    bl_label = "Engine"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return _in_mode(context) and context.tool_settings.sculpt.brush is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        brush = context.tool_settings.sculpt.brush

        if brush.sculpt_brush_type not in mapping.KERNEL_BY_TYPE:
            layout.label(text="Brush type not yet mapped", icon='ERROR')

        names = engine_props.props_for_type(brush.sculpt_brush_type)
        group = getattr(brush, "sculptcore", None)
        if names and group is not None:
            col = layout.column()
            for name in names:
                col.prop(group, name)

        if brush.texture is not None:
            from . import texture as texture_mod
            if brush.texture_slot.map_mode not in texture_mod._COORD_SPACE:
                layout.label(text="Texture mapping not supported by the engine", icon='INFO')


class SCULPTCORE_PT_automasking(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = _CATEGORY
    bl_context = _MODE_CONTEXT
    bl_label = "Automasking"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return _in_mode(context) and context.tool_settings.sculpt.brush is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        paint = context.tool_settings.sculpt
        brush = paint.brush
        settings = brush.mesh_automasking_settings

        # Only cavity is mapped; the brush's own settings take precedence over
        # the Paint-level ones (Blender's rule), so edit them here.
        col = layout.column(align=True)
        col.prop(settings, "use_automasking_cavity", text="Cavity")
        col.prop(settings, "use_automasking_cavity_inverted", text="Cavity (Inverted)")

        active = mapping.cavity_settings(brush, paint)
        col = layout.column(align=True)
        col.active = active is not None
        col.prop(settings, "cavity_factor", text="Factor")
        col.prop(settings, "cavity_blur_steps", text="Blur")
        col.prop(settings, "use_automasking_custom_cavity_curve", text="Custom Curve")
        if settings.use_automasking_custom_cavity_curve:
            layout.template_curve_mapping(settings, "cavity_curve", brush=True)

        if active is not None and active != settings:
            layout.label(text="Driven by the tool settings", icon='INFO')
        layout.label(text="Other automasking modes are not mapped", icon='INFO')


class SCULPTCORE_PT_symmetry(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = _CATEGORY
    bl_context = _MODE_CONTEXT
    bl_label = "Symmetry"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return _in_mode(context)

    def draw(self, context):
        layout = self.layout
        # Plane-mirror symmetry across the object's local axes — the same mesh
        # flags vanilla sculpt mirrors across, read by the stroke operator.
        mesh = context.active_object.data
        row = layout.row(align=True)
        row.prop(mesh, "use_mirror_x", text="X", toggle=True)
        row.prop(mesh, "use_mirror_y", text="Y", toggle=True)
        row.prop(mesh, "use_mirror_z", text="Z", toggle=True)


class SCULPTCORE_PT_dyntopo(bpy.types.Panel):
    """Vanilla VIEW3D_PT_sculpt_dyntopo's shape over Blender's own detail
    settings (which the stroke consumes via stroke.dyntopo_max_edge), plus
    the engine's remesh-cadence property. Omitted vs vanilla (allowlisted):
    the sample-detail eyedropper and detail flood fill."""
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = _CATEGORY
    bl_context = _MODE_CONTEXT
    bl_label = "Dyntopo"
    bl_options = {'DEFAULT_CLOSED'}
    bl_ui_units_x = 12

    @classmethod
    def poll(cls, context):
        return _in_mode(context)

    def draw_header(self, context):
        self.layout.prop(context.scene, "sculptcore_dyntopo", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        sculpt = context.tool_settings.sculpt

        col = layout.column()
        col.active = context.scene.sculptcore_dyntopo
        if sculpt.detail_type_method in {'CONSTANT', 'MANUAL'}:
            col.prop(sculpt, "constant_detail_resolution")
        elif sculpt.detail_type_method == 'BRUSH':
            col.prop(sculpt, "detail_percent")
        else:
            col.prop(sculpt, "detail_size")
        col.prop(sculpt, "detail_refine_method", text="Refine Method")
        col.prop(sculpt, "detail_type_method", text="Detailing")
        col.prop(context.scene, "sculptcore_dyntopo_spacing")

        # Engine remesher tuning (DynTopoParams).
        scene = context.scene
        col.separator()
        sub = col.column(heading="Remesher")
        sub.prop(scene, "sculptcore_dyntopo_flips")
        sub.prop(scene, "sculptcore_dyntopo_smooth")
        row = sub.row()
        row.active = scene.sculptcore_dyntopo_smooth
        row.prop(scene, "sculptcore_dyntopo_smooth_lambda")
        sub = col.column()
        sub.prop(scene, "sculptcore_dyntopo_max_rounds")
        sub.prop(scene, "sculptcore_dyntopo_split_budget")
        sub.prop(scene, "sculptcore_dyntopo_collapse_budget")


class SCULPTCORE_PT_boundary_uv(bpy.types.Panel):
    """Boundary/UV tools: project a UV map from the marked seam edges (the
    engine unwrapper; seams/sharp edges migrate from the Mesh on enter and
    constrain the boundary-aware smooth)."""
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = _CATEGORY
    bl_context = _MODE_CONTEXT
    bl_label = "Boundary"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        if not _in_mode(context):
            return False
        session = engine.sessions.get(context.active_object.name)
        return session is not None and session.multires_ptr is None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        col = layout.column()
        col.prop(context.scene, "sculptcore_reproject_uvs")
        col.separator()
        col.prop(context.scene, "sculptcore_uv_margin")
        props = col.operator("sculptcore.uv_project_from_seams")
        props.margin = context.scene.sculptcore_uv_margin


class SCULPTCORE_PT_multires(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = _CATEGORY
    bl_context = _MODE_CONTEXT
    bl_label = "Multires"

    @classmethod
    def poll(cls, context):
        if not _in_mode(context):
            return False
        session = engine.sessions.get(context.active_object.name)
        return session is not None and session.multires_ptr is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        ob = context.active_object
        md = multires.modifier(ob)
        if md is None:
            return
        # The handler follows this property and switches the engine level.
        layout.prop(md, "sculpt_levels", text="Sculpt Level")
        session = engine.sessions.get(ob.name)
        if session is not None and md.sculpt_levels < 1:
            layout.label(text="Level 0 sculpts at level 1", icon='INFO')


def _make_asset_shelf():
    """The brush asset shelf, from the same (unregistered) mixins vanilla's
    VIEW3D_AST_brush_sculpt uses. Brushes are shared assets; only the poll
    differs (this mode instead of the built-in sculpt mode)."""
    from bl_ui.space_view3d import View3DAssetShelf

    class SCULPTCORE_AST_brush_sculpt(View3DAssetShelf, bpy.types.AssetShelf):
        mode = 'CUSTOM'
        mode_prop = "use_paint_sculpt"
        brush_type_prop = "sculpt_brush_type"

        @classmethod
        def poll(cls, context):
            return _in_mode(context)

    return SCULPTCORE_AST_brush_sculpt


def _tool_header_mode_settings(self, context):
    """Tool-header extras, like vanilla sculpt's: the brush child-panel
    popovers (registered child panels never render inside their parent's
    popover, so vanilla exposes them individually — draw_3d_brush_settings)
    and the mirror-axis row. Top-level Tool panels become popovers via the
    generic custom-mode popover_group in VIEW3D_HT_tool_header."""
    if not _in_mode(context):
        return
    layout = self.layout
    if context.tool_settings.sculpt.brush is not None:
        layout.popover("SCULPTCORE_PT_tools_brush_settings_advanced", text="Brush")
        layout.popover("SCULPTCORE_PT_tools_brush_texture")
        layout.popover("SCULPTCORE_PT_tools_brush_stroke")
        layout.popover("SCULPTCORE_PT_tools_brush_falloff")
        layout.popover("SCULPTCORE_PT_tools_brush_display")
    row = layout.row(align=True)
    row.label(text="Mirror")
    mesh = context.active_object.data
    sub = row.row(align=True)
    sub.prop(mesh, "use_mirror_x", text="X", toggle=True)
    sub.prop(mesh, "use_mirror_y", text="Y", toggle=True)
    sub.prop(mesh, "use_mirror_z", text="Z", toggle=True)


_classes = (
    SCULPTCORE_PT_brush_engine,
    SCULPTCORE_PT_automasking,
    SCULPTCORE_PT_symmetry,
    SCULPTCORE_PT_dyntopo,
    SCULPTCORE_PT_boundary_uv,
    SCULPTCORE_PT_multires,
)


_asset_shelf_cls = None


def register():
    global _asset_shelf_cls
    for cls in _classes:
        bpy.utils.register_class(cls)
    _asset_shelf_cls = _make_asset_shelf()
    bpy.utils.register_class(_asset_shelf_cls)
    bpy.types.VIEW3D_HT_tool_header.append(_tool_header_mode_settings)


def unregister():
    global _asset_shelf_cls
    bpy.types.VIEW3D_HT_tool_header.remove(_tool_header_mode_settings)
    if _asset_shelf_cls is not None:
        bpy.utils.unregister_class(_asset_shelf_cls)
        _asset_shelf_cls = None
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
