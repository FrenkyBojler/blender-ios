/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_function_util.hh"

namespace blender::nodes::node_fn_string_to_int_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("String").hide_label();
  b.add_output<decl::Int>("Integer");
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto to_int_fn = mf::build::SI1_SO<std::string, int>(
      "String to Integer",
      [](const std::string &a) {
        try {
          return std::stoi(a);
        }
        catch (...) {
          return 0;
        }
      });

  builder.set_matching_fn(&to_int_fn);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeStringToInt");
  ntype.ui_name = "String to Integer";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_string_to_int_cc
