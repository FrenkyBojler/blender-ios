/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_function_util.hh"

#include "NOD_socket_search_link.hh"

#include <charconv>

namespace blender::nodes::node_fn_hex_to_integer_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::String>("Hex"_ustr).optional_label();
  b.add_output<decl::Int>("Integer"_ustr);
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI1_SO<std::string, int>(
      "Hex to Integer", [](const std::string &s) -> int {
        const char *begin = s.data();
        const char *end = begin + s.size();
        /* Strip optional 0x, 0X, or # prefix. */
        if (s.size() >= 2 && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X')) {
          begin += 2;
        }
        else if (!s.empty() && begin[0] == '#') {
          begin += 1;
        }
        uint value = 0;
        std::from_chars(begin, end, value, 16);
        return int(value);
      });
  builder.set_matching_fn(&fn);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN && socket_type == SOCK_STRING) {
    params.add_item(IFACE_("Hex"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("FunctionNodeHexToInteger"_ustr);
      params.update_and_connect_available_socket(node, "Hex"_ustr);
    });
  }
  else if (params.in_out() == SOCK_OUT && ELEM(socket_type, SOCK_INT, SOCK_BOOLEAN)) {
    params.add_item(IFACE_("Integer"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("FunctionNodeHexToInteger"_ustr);
      params.update_and_connect_available_socket(node, "Integer"_ustr);
    });
  }
}

static void node_register()
{
  static bke::bNodeType ntype;

  fn_cmp_node_type_base(&ntype, "FunctionNodeHexToInteger"_ustr);
  ntype.ui_name = "Hex to Integer";
  ntype.ui_description = "Parse a hexadecimal string as an integer value";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  ntype.gather_link_search_ops = node_gather_link_searches;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_hex_to_integer_cc
