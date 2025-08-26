# SPDX-FileCopyrightText: 2022-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Menu
from bl_ui import node_add_menu
from bpy.app.translations import (
    contexts as i18n_contexts,
)


class NODE_MT_compositor_node_input_base(Menu):
    bl_label = "Input"

    def draw(self, context):
        del context
        layout = self.layout
        self.draw_menu(layout, path="Input/Constant")
        layout.separator()
        self.node_operator(layout, "NodeGroupInput")
        self.node_operator(layout, "CompositorNodeBokehImage")
        self.node_operator(layout, "CompositorNodeImage")
        self.node_operator(layout, "CompositorNodeImageInfo")
        self.node_operator(layout, "CompositorNodeImageCoordinates")
        self.node_operator(layout, "CompositorNodeMask")
        self.node_operator(layout, "CompositorNodeMovieClip")

        layout.separator()
        self.draw_menu(layout, path="Input/Scene")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_input_constant_base(Menu):
    bl_label = "Constant"
    menu_path = "Input/Constant"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodeRGB")
        self.node_operator(layout, "ShaderNodeValue")
        self.node_operator(layout, "CompositorNodeNormal")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_compositor_node_input_scene_base(Menu):
    bl_label = "Scene"
    menu_path = "Input/Scene"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodeRLayers")
        self.node_operator_with_outputs(context, layout, "CompositorNodeSceneTime", ["Frame", "Seconds"])
        self.node_operator(layout, "CompositorNodeTime")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_compositor_node_output_base(Menu):
    bl_label = "Output"

    def draw(self, context):
        del context
        layout = self.layout
        self.node_operator(layout, "NodeGroupOutput")
        self.node_operator(layout, "CompositorNodeViewer")
        layout.separator()
        self.node_operator(layout, "CompositorNodeOutputFile")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_color_base(Menu):
    bl_label = "Color"

    def draw(self, _context):
        layout = self.layout
        self.draw_menu(layout, path="Color/Adjust")
        layout.separator()
        self.draw_menu(layout, path="Color/Mix")
        layout.separator()
        self.node_operator(layout, "CompositorNodePremulKey")
        self.node_operator(layout, "ShaderNodeBlackbody")
        self.node_operator(layout, "ShaderNodeValToRGB")
        self.node_operator(layout, "CompositorNodeConvertColorSpace")
        self.node_operator(layout, "CompositorNodeSetAlpha")
        layout.separator()
        self.node_operator(layout, "CompositorNodeInvert")
        self.node_operator(layout, "CompositorNodeRGBToBW")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_color_adjust_base(Menu):
    bl_label = "Adjust"
    menu_path = "Color/Adjust"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodeBrightContrast")
        self.node_operator(layout, "CompositorNodeColorBalance")
        self.node_operator(layout, "CompositorNodeColorCorrection")
        self.node_operator(layout, "CompositorNodeExposure")
        self.node_operator(layout, "ShaderNodeGamma")
        self.node_operator(layout, "CompositorNodeHueCorrect")
        self.node_operator(layout, "CompositorNodeHueSat")
        self.node_operator(layout, "CompositorNodeCurveRGB")
        self.node_operator(layout, "CompositorNodeTonemap")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_compositor_node_color_mix_base(Menu):
    bl_label = "Mix"
    menu_path = "Color/Mix"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodeAlphaOver")
        layout.separator()
        self.node_operator(layout, "CompositorNodeCombineColor")
        self.node_operator(layout, "CompositorNodeSeparateColor")
        layout.separator()
        self.node_operator(layout, "CompositorNodeZcombine")
        self.color_mix_node(context, layout)
        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_compositor_node_filter_base(Menu):
    bl_label = "Filter"

    def draw(self, context):
        layout = self.layout
        self.draw_menu(layout, path="Filter/Blur")
        layout.separator()
        self.node_operator(layout, "CompositorNodeAntiAliasing")
        self.node_operator(layout, "CompositorNodeDenoise")
        self.node_operator(layout, "CompositorNodeDespeckle")
        layout.separator()
        self.node_operator(layout, "CompositorNodeDilateErode")
        self.node_operator(layout, "CompositorNodeInpaint")
        layout.separator()
        self.node_operator_with_searchable_enum(context, layout, "CompositorNodeFilter", "filter_type")
        self.node_operator_with_searchable_enum(context, layout, "CompositorNodeGlare", "glare_type")
        self.node_operator(layout, "CompositorNodeKuwahara")
        self.node_operator(layout, "CompositorNodePixelate")
        self.node_operator(layout, "CompositorNodePosterize")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_filter_blur_base(Menu):
    bl_label = "Blur"
    menu_path = "Filter/Blur"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodeBilateralblur")
        self.node_operator(layout, "CompositorNodeBlur")
        self.node_operator(layout, "CompositorNodeBokehBlur")
        self.node_operator(layout, "CompositorNodeDefocus")
        self.node_operator(layout, "CompositorNodeDBlur")
        self.node_operator(layout, "CompositorNodeVecBlur")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_compositor_node_keying_base(Menu):
    bl_label = "Keying"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodeChannelMatte")
        self.node_operator(layout, "CompositorNodeChromaMatte")
        self.node_operator(layout, "CompositorNodeColorMatte")
        self.node_operator(layout, "CompositorNodeColorSpill")
        self.node_operator(layout, "CompositorNodeDiffMatte")
        self.node_operator(layout, "CompositorNodeDistanceMatte")
        self.node_operator(layout, "CompositorNodeKeying")
        self.node_operator(layout, "CompositorNodeKeyingScreen")
        self.node_operator(layout, "CompositorNodeLumaMatte")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_mask_base(Menu):
    bl_label = "Mask"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodeCryptomatteV2")
        self.node_operator(layout, "CompositorNodeCryptomatte")
        layout.separator()
        self.node_operator(layout, "CompositorNodeBoxMask")
        self.node_operator(layout, "CompositorNodeEllipseMask")
        layout.separator()
        self.node_operator(layout, "CompositorNodeDoubleEdgeMask")
        self.node_operator(layout, "CompositorNodeIDMask")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_tracking_base(Menu):
    bl_label = "Tracking"
    bl_translation_context = i18n_contexts.id_movieclip

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodePlaneTrackDeform")
        self.node_operator(layout, "CompositorNodeStabilize")
        self.node_operator(layout, "CompositorNodeTrackPos")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_transform_base(Menu):
    bl_label = "Transform"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodeRotate")
        self.node_operator(layout, "CompositorNodeScale")
        self.node_operator(layout, "CompositorNodeTransform")
        self.node_operator(layout, "CompositorNodeTranslate")
        layout.separator()
        self.node_operator(layout, "CompositorNodeCornerPin")
        self.node_operator(layout, "CompositorNodeCrop")
        layout.separator()
        self.node_operator(layout, "CompositorNodeDisplace")
        self.node_operator(layout, "CompositorNodeFlip")
        self.node_operator(layout, "CompositorNodeMapUV")
        layout.separator()
        self.node_operator(layout, "CompositorNodeLensdist")
        self.node_operator(layout, "CompositorNodeMovieDistortion")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_texture_base(Menu):
    bl_label = "Texture"

    def draw(self, _context):
        layout = self.layout

        self.node_operator(layout, "ShaderNodeTexBrick")
        self.node_operator(layout, "ShaderNodeTexChecker")
        self.node_operator(layout, "ShaderNodeTexGabor")
        self.node_operator(layout, "ShaderNodeTexGradient")
        self.node_operator(layout, "ShaderNodeTexMagic")
        self.node_operator(layout, "ShaderNodeTexNoise")
        self.node_operator(layout, "ShaderNodeTexVoronoi")
        self.node_operator(layout, "ShaderNodeTexWave")
        self.node_operator(layout, "ShaderNodeTexWhiteNoise")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_utilities_base(Menu):
    bl_label = "Utilities"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "ShaderNodeMapRange")
        self.node_operator_with_searchable_enum(context, layout, "ShaderNodeMath", "operation")
        self.node_operator(layout, "ShaderNodeMix")
        self.node_operator(layout, "ShaderNodeClamp")
        self.node_operator(layout, "ShaderNodeFloatCurve")
        layout.separator()
        self.node_operator(layout, "CompositorNodeLevels")
        self.node_operator(layout, "CompositorNodeNormalize")
        layout.separator()
        self.node_operator(layout, "CompositorNodeSplit")
        self.node_operator(layout, "CompositorNodeSwitch")
        self.node_operator(layout, "GeometryNodeMenuSwitch")
        node_add_menu.add_node_type(
            layout, "CompositorNodeSwitchView",
            label="Switch Stereo View")
        layout.separator()
        self.node_operator(layout, "CompositorNodeRelativeToPixel")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_compositor_node_vector_base(Menu):
    bl_label = "Vector"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "ShaderNodeCombineXYZ")
        self.node_operator(layout, "ShaderNodeSeparateXYZ")
        layout.separator()
        props = self.node_operator(layout, "ShaderNodeMix", label="Mix Vector")
        ops = props.settings.add()
        ops.name = "data_type"
        ops.value = "'VECTOR'"
        self.node_operator(layout, "ShaderNodeVectorCurve")
        self.node_operator_with_searchable_enum(context, layout, "ShaderNodeVectorMath", "operation")
        self.node_operator(layout, "ShaderNodeVectorRotate")

        if self.draw_assets:
            node_add_menu.draw_assets_for_catalog(layout, self.bl_label)


add_menus = {
    # menu bl_idname: baseclass
    "NODE_MT_category_compositor_input": NODE_MT_compositor_node_input_base,
    "NODE_MT_category_compositor_input_constant": NODE_MT_compositor_node_input_constant_base,
    "NODE_MT_category_compositor_input_scene": NODE_MT_compositor_node_input_scene_base,
    "NODE_MT_category_compositor_output": NODE_MT_compositor_node_output_base,
    "NODE_MT_category_compositor_color": NODE_MT_compositor_node_color_base,
    "NODE_MT_category_compositor_color_adjust": NODE_MT_compositor_node_color_adjust_base,
    "NODE_MT_category_compositor_color_mix": NODE_MT_compositor_node_color_mix_base,
    "NODE_MT_category_compositor_filter": NODE_MT_compositor_node_filter_base,
    "NODE_MT_category_compositor_filter_blur": NODE_MT_compositor_node_filter_blur_base,
    "NODE_MT_category_compositor_texture": NODE_MT_compositor_node_texture_base,
    "NODE_MT_category_compositor_keying": NODE_MT_compositor_node_keying_base,
    "NODE_MT_category_compositor_mask": NODE_MT_compositor_node_mask_base,
    "NODE_MT_category_compositor_tracking": NODE_MT_compositor_node_tracking_base,
    "NODE_MT_category_compositor_transform": NODE_MT_compositor_node_transform_base,
    "NODE_MT_category_compositor_utilities": NODE_MT_compositor_node_utilities_base,
    "NODE_MT_category_compositor_vector": NODE_MT_compositor_node_vector_base,
}
add_menus = node_add_menu.generate_menus(add_menus, template=node_add_menu.AddNodeMenu)


class NODE_MT_compositor_node_add_all(Menu):
    bl_label = ""

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_category_compositor_input")
        layout.menu("NODE_MT_category_compositor_output")
        layout.separator()
        layout.menu("NODE_MT_category_compositor_color")
        layout.menu("NODE_MT_category_compositor_filter")
        layout.separator()
        layout.menu("NODE_MT_category_compositor_keying")
        layout.menu("NODE_MT_category_compositor_mask")
        layout.separator()
        layout.menu("NODE_MT_category_compositor_tracking")
        layout.separator()
        layout.menu("NODE_MT_category_compositor_texture")
        layout.menu("NODE_MT_category_compositor_transform")
        layout.menu("NODE_MT_category_compositor_utilities")
        layout.menu("NODE_MT_category_compositor_vector")
        layout.separator()
        layout.menu("NODE_MT_group_add")
        layout.menu("NODE_MT_category_layout")

        node_add_menu.draw_root_assets(layout)


swap_menus = {
    # menu bl_idname: baseclass
    "NODE_MT_compositor_node_input_swap": NODE_MT_compositor_node_input_base,
    "NODE_MT_compositor_node_input_constant_swap": NODE_MT_compositor_node_input_constant_base,
    "NODE_MT_compositor_node_input_scene_swap": NODE_MT_compositor_node_input_scene_base,
    "NODE_MT_compositor_node_output_swap": NODE_MT_compositor_node_output_base,
    "NODE_MT_compositor_node_color_swap": NODE_MT_compositor_node_color_base,
    "NODE_MT_compositor_node_color_adjust_swap": NODE_MT_compositor_node_color_adjust_base,
    "NODE_MT_compositor_node_color_mix_swap": NODE_MT_compositor_node_color_mix_base,
    "NODE_MT_compositor_node_filter_swap": NODE_MT_compositor_node_filter_base,
    "NODE_MT_compositor_node_filter_blur_swap": NODE_MT_compositor_node_filter_blur_base,
    "NODE_MT_compositor_node_texture_swap": NODE_MT_compositor_node_texture_base,
    "NODE_MT_compositor_node_keying_swap": NODE_MT_compositor_node_keying_base,
    "NODE_MT_compositor_node_mask_swap": NODE_MT_compositor_node_mask_base,
    "NODE_MT_compositor_node_tracking_swap": NODE_MT_compositor_node_tracking_base,
    "NODE_MT_compositor_node_transform_swap": NODE_MT_compositor_node_transform_base,
    "NODE_MT_compositor_node_utilities_swap": NODE_MT_compositor_node_utilities_base,
    "NODE_MT_compositor_node_vector_swap": NODE_MT_compositor_node_vector_base,
}
swap_menus = node_add_menu.generate_menus(swap_menus, template=node_add_menu.SwapNodeMenu)


class NODE_MT_compositor_node_swap_all(Menu):
    bl_label = ""

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_compositor_node_input_swap")
        layout.menu("NODE_MT_compositor_node_output_swap")
        layout.separator()
        layout.menu("NODE_MT_compositor_node_color_swap")
        layout.menu("NODE_MT_compositor_node_filter_swap")
        layout.separator()
        layout.menu("NODE_MT_compositor_node_keying_swap")
        layout.menu("NODE_MT_compositor_node_mask_swap")
        layout.separator()
        layout.menu("NODE_MT_compositor_node_tracking_swap")
        layout.separator()
        layout.menu("NODE_MT_compositor_node_transform_swap")
        layout.menu("NODE_MT_compositor_node_utilities_swap")
        layout.menu("NODE_MT_compositor_node_utilities_swap")
        layout.menu("NODE_MT_compositor_node_vector_swap")
        layout.separator()
        layout.menu("NODE_MT_group_swap")
        layout.menu("NODE_MT_layout_swap")

        # node_add_menu.draw_root_assets(layout)


classes = (
    NODE_MT_compositor_node_add_all,
    *add_menus,
    NODE_MT_compositor_node_swap_all,
    *swap_menus,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
