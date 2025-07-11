/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "NOD_multi_function.hh"

#include "FN_multi_function_builder.hh"

#include "BKE_node.hh"

#include "NOD_register.hh"

using namespace blender;

namespace blender::nodes::node_fn_gamma_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.is_function_node();
  b.add_input<decl::Color>("Color")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .description("Color input on which correction will be applied");
  b.add_output<decl::Color>("Color").align_with_previous();
  b.add_input<decl::Float>("Gamma")
      .default_value(1.0f)
      .min(0.001f)
      .max(10.0f)
      .subtype(PROP_UNSIGNED)
      .description(
          "Gamma correction value, applied as color^gamma.\n"
          "Gamma controls the relative intensity of the mid-tones compared to the full black and "
          "full white");
}

using namespace blender::math;

static void node_build_multi_function(blender::nodes::NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI2_SO<float4, float, ColorGeometry4f>(
      "Gamma",
      [](const float4 &color, float gamma) -> ColorGeometry4f {
        float4 gamma_applied = float4(math::safe_pow(color.xyz(), gamma), color.w);
        return ColorGeometry4f(gamma_applied.x, gamma_applied.y, gamma_applied.z, gamma_applied.w);
      },
      mf::build::exec_presets::AllSpanOrSingle());
  builder.set_matching_fn(fn);
}

}  // namespace blender::nodes::node_fn_gamma_cc

static void node_register()
{
  namespace file_ns = blender::nodes::node_fn_gamma_cc;

  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeGamma");
  ntype.ui_name = "Gamma";
  ntype.ui_description = "Apply a gamma correction to a color";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = file_ns::node_declare;
  ntype.build_multi_function = file_ns::node_build_multi_function;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
