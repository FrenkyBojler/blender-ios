/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_string_utf8.h"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_string_case_cc {

enum class Case {
  Uppercase = 0,
  Lowercase = 1,
  TitleCase = 2,
  Capitalize = 3,
};

static const EnumPropertyItem case_items[] = {
    {int(Case::Uppercase), "UPPERCASE", 0, "Uppercase", "Convert all characters to uppercase"},
    {int(Case::Lowercase), "LOWERCASE", 0, "Lowercase", "Convert all characters to lowercase"},
    {int(Case::TitleCase),
     "TITLE_CASE",
     0,
     "Title Case",
     "Capitalize the first letter of each word"},
    {int(Case::Capitalize),
     "CAPITALIZE",
     0,
     "Capitalize",
     "Capitalize only the first character of the string, leaving the rest unchanged"},
    {},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::String>("String"_ustr).optional_label();
  b.add_output<decl::String>("String"_ustr).align_with_previous();
  b.add_input<decl::Menu>("Case"_ustr).static_items(case_items).optional_label();
}

static std::string apply_string_case(const std::string &s, const Case mode)
{
  if (s.empty()) {
    return s;
  }

  size_t len_bytes;
  const size_t len_chars = BLI_strlen_utf8_ex(s.c_str(), &len_bytes);

  Array<char32_t> utf32(len_chars + 1);
  BLI_str_utf8_as_utf32(utf32.data(), s.c_str(), utf32.size());

  bool word_start = true;

  for (size_t i = 0; i < len_chars; i++) {
    const char32_t c = utf32[i];
    switch (mode) {
      case Case::Uppercase:
        utf32[i] = BLI_str_utf32_char_to_upper(c);
        break;
      case Case::Lowercase:
        utf32[i] = BLI_str_utf32_char_to_lower(c);
        break;
      case Case::TitleCase: {
        if (std::isspace(c)) {
          word_start = true;
        }
        else if (word_start) {
          utf32[i] = BLI_str_utf32_char_to_upper(c);
          word_start = false;
        }
        else {
          utf32[i] = BLI_str_utf32_char_to_lower(c);
        }
        break;
      }
      case Case::Capitalize:
        if (i == 0) {
          utf32[i] = BLI_str_utf32_char_to_upper(c);
        }
        break;
    }
  }

  std::vector<char> out(len_chars * 4 + 1);
  BLI_str_utf32_as_utf8(out.data(), utf32.data(), out.size());
  return std::string(out.data());
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto fn = mf::build::SI2_SO<std::string, MenuValue, std::string>(
      "String Case", [](const std::string &s, MenuValue mode) -> std::string {
        return apply_string_case(s, Case(mode.value));
      });
  builder.set_matching_fn(&fn);
}

static void node_register()
{
  static bke::bNodeType ntype;

  fn_cmp_node_type_base(&ntype, "FunctionNodeStringCase"_ustr);
  ntype.ui_name = "String Case";
  ntype.ui_description = "Convert the case of a string";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_string_case_cc
