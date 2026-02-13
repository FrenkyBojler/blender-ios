/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_function_util.hh"

namespace blender::nodes::node_fn_trim_string_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::String>("String").optional_label();
  b.add_output<decl::String>("String").align_with_previous();
  b.add_input<decl::String>("Characters").optional_label();
  b.add_input<decl::Bool>("Whitespace").default_value(true);
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto trim_fn = mf::build::SI3_SO<std::string, bool, std::string, std::string>(
      "Trim",
      [](const std::string &input_str, const bool trim_whitespace, const std::string &characters) {
        std::string characters_to_trim = characters;
        if (trim_whitespace) {
          characters_to_trim.append(" \t\n\r");
        }
        std::string result = StringRef(input_str).trim(characters_to_trim);
        return result;
      });
  builder.set_matching_fn(&trim_fn);
}

static void node_register()
{
  static bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeTrimString");
  ntype.ui_name = "Trim String";
  ntype.ui_description = "Remove characters whitespace from the beginning and end of a string";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_trim_string_cc
