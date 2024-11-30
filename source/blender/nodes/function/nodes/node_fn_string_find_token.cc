/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_utf8.h"
#include "node_function_util.hh"
#include <charconv>
#include <iomanip>

namespace blender::nodes::node_fn_string_find_token_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("String").hide_label();
  b.add_input<decl::String>("Token");
  b.add_input<decl::Int>("Start Char").min(0);
  b.add_input<decl::Int>("Next Find").min(0).default_value(1);
  b.add_input<decl::Bool>("Overlap Matches");
  b.add_output<decl::Int>("Token Position");
  b.add_output<decl::Int>("Token Count");
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

static std::vector<int> string_find_tokens(const StringRef text,
                                           const StringRef token,
                                           bool overlap)
{
  std::vector<int> positions;
   if (text.is_empty() || token.is_empty()) {
    return positions;
   }
  std::u32string a_u32 = bli_str_utf8_as_u32string(text);
  std::u32string b_u32 = bli_str_utf8_as_u32string(token);

  int Matche_len;
  if (!overlap) {
    Matche_len = b_u32.size();
  }
  else {
    Matche_len = 1;
  }
  int pos = 0;
  while ((pos = a_u32.find(b_u32, pos)) != std::u32string::npos) {
    positions.push_back(pos);
    pos += Matche_len;
  }
  return positions;
}
static int out_finded_token_position(const std::vector<int> *positions, const int start, const int next)
{
  if (positions->empty()||start<0||next<=0) {
    return 0;
  }
  if (start > positions->back()) {
    return -1;
  }
  auto it = std::lower_bound(positions->begin(), positions->end(), start + 1);
  if (it == positions->end()) {
    return -1;
  }
  int index = std::distance(positions->begin(), it) + next;
  if (index > positions->size()) {
    return -1;
  }
  return (*positions)[index - 1];
}
static int out_finded_tokens_count(const std::vector<int> *positions, const int start)
{
  if (positions->empty()) {
    return 0;
  }
  if (start > positions->back()) {
    return 0;
  }
  auto it = std::lower_bound(positions->begin(), positions->end(), start);
  if (it == positions->end()) {
    return 0;
  }
  return static_cast<int>(positions->end() - it);
}

static void string_find_count_out(const StringRef text,
                                  const StringRef token,
                                  const int start,
                                  const int next,
                                  const bool overlap,
                                  int &out_pos,
                                  int &out_count)
{
  std::vector<int> positions = string_find_tokens(text, token, overlap);
  out_pos = out_finded_token_position(&positions, start, next);
  out_count = out_finded_tokens_count(&positions, start);
}
static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto token_position_count =
      mf::build::SI5_SO2<std::string, std::string, int, int, bool,int, int>(
          "String Find Token",
          [](const std::string &text,
             const std::string &token,
             const int &start,
             const int &next,
             const bool &overlap,
             int &position,
             int &count) -> void {
            string_find_count_out(text,token,start,next,overlap,position,count);
          },
          mf::build::exec_presets::AllSpanOrSingle());

  builder.set_matching_fn(&token_position_count);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, FN_NODE_STRING_FIND_TOKEN, "String Find Token", NODE_CLASS_CONVERTER);
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_string_find_token_cc
