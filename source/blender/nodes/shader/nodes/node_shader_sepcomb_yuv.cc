/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "BLI_math_color.h"
#include "node_shader_util.hh"

#include "FN_multi_function_builder.hh"

#include "NOD_multi_function.hh"

namespace blender::nodes::node_shader_sepcomb_yuv_cc {

static void sh_node_sepyuv_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Color>("Image").default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_output<decl::Float>("Y").translation_context(BLT_I18NCONTEXT_COLOR);
  b.add_output<decl::Float>("U").translation_context(BLT_I18NCONTEXT_COLOR);
  b.add_output<decl::Float>("V").translation_context(BLT_I18NCONTEXT_COLOR);
}

static int gpu_shader_sepyuv(GPUMaterial *mat,
                             bNode *node,
                             bNodeExecData * /*execdata*/,
                             GPUNodeStack *in,
                             GPUNodeStack *out)
{
  // TODO: the shader has to be created
  return GPU_stack_link(mat, node, "separate_yuv", in, out);
}

class SeparateYUVFunction : public mf::MultiFunction {
 public:
  SeparateYUVFunction()
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Separate YUV", signature};
      builder.single_input<ColorGeometry4f>("Color");
      builder.single_output<float>("Y");
      builder.single_output<float>("U");
      builder.single_output<float>("V");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<ColorGeometry4f> &colors = params.readonly_single_input<ColorGeometry4f>(0,
                                                                                          "Color");
    MutableSpan<float> rs = params.uninitialized_single_output<float>(1, "Y");
    MutableSpan<float> gs = params.uninitialized_single_output<float>(2, "U");
    MutableSpan<float> bs = params.uninitialized_single_output<float>(3, "V");

    mask.foreach_index([&](const int64_t i) {
      ColorGeometry4f color = colors[i];
      rgb_to_yuv(color.r, color.g, color.b, &rs[i], &gs[i], &bs[i], BLI_YUV_ITU_BT709);
    });
  }
};

static void sh_node_sepyuv_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static SeparateYUVFunction fn;
  builder.set_matching_fn(fn);
}

}  // namespace blender::nodes::node_shader_sepcomb_yuv_cc

void register_node_type_sh_sepyuv()
{
  namespace file_ns = blender::nodes::node_shader_sepcomb_yuv_cc;

  static blender::bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeSeparateYUV", SH_NODE_SEPYUV_LEGACY);
  ntype.ui_name = "Separate YUV (Legacy)";
  ntype.ui_description = "Deprecated";
  ntype.enum_name_legacy = "SEPYUV";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::sh_node_sepyuv_declare;
  ntype.gpu_fn = file_ns::gpu_shader_sepyuv;
  ntype.build_multi_function = file_ns::sh_node_sepyuv_build_multi_function;
  ntype.gather_link_search_ops = nullptr;

  blender::bke::node_register_type(ntype);
}

namespace blender::nodes::node_shader_sepcomb_yuv_cc {

static void sh_node_combyuv_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("R").min(0.0f).max(1.0f).translation_context(BLT_I18NCONTEXT_COLOR);
  b.add_input<decl::Float>("G").min(0.0f).max(1.0f).translation_context(BLT_I18NCONTEXT_COLOR);
  b.add_input<decl::Float>("B").min(0.0f).max(1.0f).translation_context(BLT_I18NCONTEXT_COLOR);
  b.add_output<decl::Color>("Image");
}

static int gpu_shader_combyuv(GPUMaterial *mat,
                              bNode *node,
                              bNodeExecData * /*execdata*/,
                              GPUNodeStack *in,
                              GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "combine_yuv", in, out);
}

static void sh_node_combyuv_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI3_SO<float, float, float, ColorGeometry4f>(
      "Combine YUV", [](float r, float g, float b) { return ColorGeometry4f(r, g, b, 1.0f); });
  builder.set_matching_fn(fn);
}

}  // namespace blender::nodes::node_shader_sepcomb_yuv_cc

void register_node_type_sh_combyuv()
{
  namespace file_ns = blender::nodes::node_shader_sepcomb_yuv_cc;

  static blender::bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeCombineYUV", SH_NODE_COMBYUV_LEGACY);
  ntype.ui_name = "Combine YUV (Legacy)";
  ntype.ui_description = "Deprecated";
  ntype.enum_name_legacy = "SEPYUV";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::sh_node_combyuv_declare;
  ntype.gpu_fn = file_ns::gpu_shader_combyuv;
  ntype.build_multi_function = file_ns::sh_node_combyuv_build_multi_function;
  ntype.gather_link_search_ops = nullptr;

  blender::bke::node_register_type(ntype);
}
