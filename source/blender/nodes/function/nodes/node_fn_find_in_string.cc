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

std::u32string bli_str_utf8_as_u32string(const StringRef u8src)
{
  std::u32string u32out;
  const size_t u8src_len = u8src.size();
  u32out.reserve(u8src_len);
  const char *src_c_end = u8src.data() + u8src_len;
  size_t index = 0;

  while (index < u8src_len) {
    const uint unicode = BLI_str_utf8_as_unicode_step_or_error(u8src.data(), u8src_len, &index);
    if (unicode != BLI_UTF8_ERR) {
      u32out.push_back(unicode);
    }
    else {
      u32out.push_back(' ');
      const char *src_c_next = BLI_str_find_next_char_utf8(u8src.data() + index, src_c_end);
      index = size_t(src_c_next - u8src.data());
    }
  }

  return u32out;
}

static Vector<int> string_find_tokens(const StringRef text, const StringRef token)
{
  Vector<int> positions;
  if (text.is_empty() || token.is_empty()) {
    return positions;
  }
  std::u32string a_u32 = bli_str_utf8_as_u32string(text);
  std::u32string b_u32 = bli_str_utf8_as_u32string(token);

  int matche_len = b_u32.size();
  int pos = 0;
  if (a_u32.substr(0, b_u32.size()) == b_u32) {
    positions.append(0);
    pos += matche_len;
  }
  while ((pos = a_u32.find(b_u32, pos)) != std::u32string::npos) {
    positions.append(pos);
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
