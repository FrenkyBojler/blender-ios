/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_function_util.hh"

#include "NOD_socket_search_link.hh"

#include <cstdio>

namespace blender::nodes::node_fn_integer_to_hex_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Int>("Integer"_ustr);
  b.add_output<decl::String>("Hex"_ustr);
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI1_SO<int, std::string>(
      "Integer to Hex", [](int value) -> std::string {
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%X", uint(value));
        return std::string(buf);
      });
  builder.set_matching_fn(&fn);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN && ELEM(socket_type, SOCK_INT, SOCK_BOOLEAN)) {
    params.add_item(IFACE_("Integer"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("FunctionNodeIntegerToHex"_ustr);
      params.update_and_connect_available_socket(node, "Integer"_ustr);
    });
  }
  else if (params.in_out() == SOCK_OUT && socket_type == SOCK_STRING) {
    params.add_item(IFACE_("Hex"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("FunctionNodeIntegerToHex"_ustr);
      params.update_and_connect_available_socket(node, "Hex"_ustr);
    });
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  fn_cmp_node_type_base(&ntype, "FunctionNodeIntegerToHex"_ustr);
  ntype.ui_name = "Integer to Hex";
  ntype.ui_description = "Format an integer value as a hexadecimal string";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  ntype.gather_link_search_ops = node_gather_link_searches;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_integer_to_hex_cc
