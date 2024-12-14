/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_utf8.h"
#include "node_function_util.hh"
#include <charconv>
#include <iomanip>

namespace blender::nodes::node_fn_find_in_string_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("String").hide_label();
  b.add_input<decl::String>("Search");
  b.add_output<decl::Int>("First Found Position");
  b.add_output<decl::Int>("Count");
}

static Vector<int> string_find_tokens(const StringRef text, const StringRef token)
{
  Vector<int> positions;
  if (text.is_empty() || token.is_empty()) {
    return positions;
  }
  int matche_len = token.size();
  int pos = 0;
  if (text.substr(0, token.size()) == token) {
    positions.append(0);
    pos += matche_len;
  }
  size_t r_len_bytes;
  while ((pos = text.find(token, pos)) != StringRef::not_found) {
    int pos_n = BLI_strnlen_utf8_ex(text.data(),pos,&r_len_bytes);
    positions.append(pos_n);
    pos += matche_len;
  }
  return positions;
}
static int out_finded_first_position(const Vector<int> *positions)
{
  if (positions->is_empty()) {
    return 0;
  }
  return positions->first();
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto token_position_count = mf::build::SI2_SO2<std::string, std::string, int, int>(
      "Find in String",
      [](const std::string &text, const std::string &token, int &first, int &count) -> void {
        Vector<int> positions = string_find_tokens(text, token);
        first = out_finded_first_position(&positions);
        count = positions.size();
      },
      mf::build::exec_presets::AllSpanOrSingle());

  builder.set_matching_fn(&token_position_count);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, FN_NODE_FIND_IN_STRING, "Find in String", NODE_CLASS_CONVERTER);
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_find_in_string_cc
