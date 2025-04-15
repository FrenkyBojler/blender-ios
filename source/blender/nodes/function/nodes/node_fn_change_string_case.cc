/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string.h"

#include "UI_interface.hh"

#include "node_function_util.hh"

#include "NOD_rna_define.hh"

namespace blender::nodes::node_fn_change_string_case {

enum class ChangeCaseOperation : int8_t { Uppercase, Lowercase };

const EnumPropertyItem rna_enum_node_match_string_items[] = {
    {int(ChangeCaseOperation::Uppercase),
     "UPPER",
     0,
     "To Uppercase",
     "Convert given string into uppercase"},
    {int(ChangeCaseOperation::Lowercase),
     "LOWER",
     0,
     "To Lowercase",
     "Convert given string into lowercase"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  b.add_input<decl::String>("String").hide_label();
  b.add_output<decl::String>("Result").align_with_previous();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(ChangeCaseOperation::Uppercase);
}

static const mf::MultiFunction *get_multi_function(const bNode &bnode)
{
  const ChangeCaseOperation operation = ChangeCaseOperation(bnode.custom1);

  switch (operation) {
    case ChangeCaseOperation::Uppercase: {
      static auto fn = mf::build::SI1_SO<std::string, std::string>(
          "To Uppercase", [](const std::string &str) {
            std::string output_str = str;
            BLI_str_toupper_ascii(&output_str[0], output_str.size());
            return output_str;
          });
      return &fn;
    }
    case ChangeCaseOperation::Lowercase: {
      static auto fn = mf::build::SI1_SO<std::string, std::string>(
          "To Lowercase", [](const std::string &str) {
            std::string output_str = str;
            BLI_str_tolower_ascii(&output_str[0], output_str.size());
            return output_str;
          });
      return &fn;
    }
  }
  BLI_assert_unreachable();
  return nullptr;
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const mf::MultiFunction *fn = get_multi_function(builder.node());
  builder.set_matching_fn(fn);
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  if (params.in_out() == SOCK_IN) {
    if (params.node_tree().typeinfo->validate_link(
            static_cast<eNodeSocketDatatype>(params.other_socket().type), SOCK_STRING))
    {
      for (const EnumPropertyItem *item = rna_enum_node_match_string_items;
           item->identifier != nullptr;
           item++)
      {
        if (item->name != nullptr && item->identifier[0] != '\0') {
          ChangeCaseOperation operation = ChangeCaseOperation(item->value);
          params.add_item(IFACE_(item->name), [operation](LinkSearchOpParams &params) {
            bNode &node = params.add_node("FunctionNodeChangeCase");
            node.custom1 = int8_t(operation);
            params.update_and_connect_available_socket(node, "String");
          });
        }
      }
    }
  }

  else {
    params.add_item(IFACE_("Result"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("FunctionNodeChangeCase");
      params.update_and_connect_available_socket(node, "Result");
    });
  }
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "operation",
                    "Operation",
                    "",
                    rna_enum_node_match_string_items,
                    NOD_inline_enum_accessors(custom1),
                    int(ChangeCaseOperation::Uppercase));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeChangeCase", FN_NODE_SLICE_STRING);
  ntype.ui_name = "Change Case";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = node_gather_link_searches;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_change_string_case
