/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "RNA_enum_types.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_format_string_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::String>("Format");
  b.add_output<decl::String>("String");
}

static const mf::MultiFunction *get_multi_function(const bNode & /*bnode*/)
{
  static auto fn = mf::build::SI1_SO<std::string, std::string>(
      "Format String", [](std::string format) { return format; });
  return &fn;
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const mf::MultiFunction *fn = get_multi_function(builder.node());
  builder.set_matching_fn(fn);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeFormatString");
  ntype.ui_name = "Format String";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_format_string_cc
