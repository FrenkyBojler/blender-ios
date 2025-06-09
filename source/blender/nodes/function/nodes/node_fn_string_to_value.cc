/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_function_util.hh"

#include "BLI_string_utf8.h"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"

#include <charconv>
#include <string>

namespace blender::nodes::node_fn_string_to_value_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.add_input<decl::String>("String").hide_label();

  if (node != nullptr) {
    const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);
    b.add_output(data_type, "Value");
  }

  b.add_output<decl::Bool>("Is Valid")
      .description("Whether the string was converted successfully");
  b.add_output<decl::Int>("Length").description("Length of the string that was converted");
}

static const mf::MultiFunction *get_multi_function(const bNode &bnode)
{
  static auto str_to_float_fn = mf::build::SI1_SO3<std::string, float, bool, int>(
      "String to Value",
      [](const std::string &string, float &value, bool &valid, int &length) -> void {
        const auto result = std::from_chars(string.data(), string.data() + string.size(), value);
        valid = result.ec == std::errc();
        length = BLI_strnlen_utf8(string.data(), result.ptr - string.data());
      });
  static auto str_to_int_fn = mf::build::SI1_SO3<std::string, int, bool, int>(
      "String to Integer",
      [](const std::string &string, int &value, bool &valid, int &length) -> void {
        const auto result = std::from_chars(string.data(), string.data() + string.size(), value);
        valid = result.ec == std::errc();
        length = BLI_strnlen_utf8(string.data(), result.ptr - string.data());
      });

  switch (bnode.custom1) {
    case SOCK_FLOAT:
      return &str_to_float_fn;
    case SOCK_INT:
      return &str_to_int_fn;
  }

  BLI_assert_unreachable();
  return nullptr;
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const mf::MultiFunction *fn = get_multi_function(builder.node());
  builder.set_matching_fn(fn);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_FLOAT;
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_rna(StructRNA *srna)
{
  static const EnumPropertyItem data_types[] = {
      {SOCK_FLOAT, "FLOAT", 0, "Float", "Floating-point value"},
      {SOCK_INT, "INT", 0, "Integer", "32-bit integer"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "",
                    data_types,
                    NOD_inline_enum_accessors(custom1),
                    SOCK_FLOAT);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeStringToValue");
  ntype.ui_name = "String to Value";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_string_to_value_cc
