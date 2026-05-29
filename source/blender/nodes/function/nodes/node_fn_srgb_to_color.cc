/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_function_util.hh"

#include "BLI_color_types.hh"
#include "BLI_color.hh"

namespace blender::nodes::node_fn_srgb_to_color_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.is_function_node();
  b.add_input<decl::Color>("Color"_ustr);
  b.add_output<decl::Color>("Color"_ustr).align_with_previous();
};

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI1_SO<ColorGeometry4f, ColorGeometry4f>("Color to sRGB", [](const ColorGeometry4f value) {
    return color::to_scene_linear(ColorTheme4f(value));
  });
  builder.set_matching_fn(fn);
}

static void node_register()
{
  static bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeSRGBToColor"_ustr);
  ntype.ui_name = "sRGB To Color";
  ntype.ui_description = "Convert sRGB value to currently used blender color space";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_srgb_to_color_cc
