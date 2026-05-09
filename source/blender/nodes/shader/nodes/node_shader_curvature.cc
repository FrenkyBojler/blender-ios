/* SPDX-FileCopyrightText: 2005 Blender Authors
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
  b.add_input<decl::Color>("Color"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Float>("Distance"_ustr).default_value(1.0f).min(0.0f).max(1000.0f);
  b.add_input<decl::Vector>("Normal"_ustr).min(-1.0f).max(1.0f).hide_value();
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Curvature"_ustr);
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
  if (!in[2].link) {
    GPU_link(mat, "world_normals_get", &in[2].link);
  }

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
  /* TODO: observed crash while rendering MaterialX_v1_38_6::ExceptionShaderGenError */
  /**
   * \code{.cc}
   * NodeItem maxdistance = get_input_value("Distance", NodeItem::Type::Float);
   * NodeItem res = create_node("curvature", NodeItem::Type::Float);
   * res.set_input("coneangle", val(90.0f));
   * res.set_input("maxdistance", maxdistance);
   * \endcode
   */
  return get_output_default(socket_out_->identifier, NodeItem::Type::Any);
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
