/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "BLI_math_base.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_curvature_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Curvature"_ustr)
      .description(
          "Average curvature of the surface within the sampled radius. "
          "Monochrome value range with concavity at the minimum, "
          "and convexity at the maximum");
  b.add_output<decl::Float>("Convexity"_ustr)
      .description(
          "Average convexity of the surface within the sampled radius. "
          "0.0 represents flat, value increases linearly with angle");
  b.add_output<decl::Float>("Concavity"_ustr)
      .description(
          "Average concavity of the surface within the sampled radius. "
          "0.0 represents flat, value increases linearly with angle");
  b.add_output<decl::Color>("Both"_ustr)
      .description(
          "Curvature values composited together with "
          "convex values in the red channel, "
          "and concave values in the green channel");

  b.add_input<decl::Float>("Radius"_ustr)
      .default_value(0.01f)
      .min(0.0f)
      .max(1000.0f)
      .description("Radius for sampling nearby surfaces");
  b.add_input<decl::Float>("Bias"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Adjust sample weight falloff by distance from the center to the given radius. "
          "Approaching 0.0 means samples near the center are weighted more heavily. "
          "0.5 means sample weight falls off linearly with distance. "
          "1.0 means samples from all distances are  weighted equally");
}

static void node_shader_buts_curvature(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "samples", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  layout.prop(ptr, "only_local", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static int node_shader_gpu_curvature(GPUMaterial *mat,
                                     bNode *node,
                                     bNodeExecData * /*execdata*/,
                                     GPUNodeStack *in,
                                     GPUNodeStack *out)
{

  GPU_material_flag_set(mat, GPU_MATFLAG_CURVATURE);

  float f_samples = divide_ceil_u(node->custom1, 4);

  return GPU_stack_link(mat, node, "node_curvature", in, out, GPU_constant(&f_samples));
}

static void node_shader_init_curvature(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = 16; /* samples */
  node->custom2 = 0;
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* NOTE: This node isn't supported by MaterialX. */
  return get_output_default(socket_out_->identifier, NodeItem::Type::Float);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_curvature_cc

/* node type definition */
void register_node_type_sh_curvature()
{
  namespace file_ns = nodes::node_shader_curvature_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeCurvature"_ustr, SH_NODE_CURVATURE);
  ntype.ui_name = "Curvature";
  ntype.ui_description =
      "Generates convexity and concavity values for mesh surface.\nNote: only supported in "
      "Cycles, and may slow down renders";
  ntype.enum_name_legacy = "CURVATURE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_curvature;
  ntype.initfunc = file_ns::node_shader_init_curvature;
  ntype.gpu_fn = file_ns::node_shader_gpu_curvature;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
