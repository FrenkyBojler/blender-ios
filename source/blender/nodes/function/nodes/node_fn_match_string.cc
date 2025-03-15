/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_utf8.h"

#include "UI_interface.hh"

#include "RNA_enum_types.hh"

#include "node_function_util.hh"

#include "NOD_rna_define.hh"

namespace blender::nodes::node_fn_match_string_cc {

typedef enum NodeMatchStringOperation {
  NODE_MATCH_STR_STARTS_WITH = 0,
  NODE_MATCH_STR_ENDS_WITH = 1,
  NODE_MATCH_STR_CONTAINS = 2,
} NodeMatchStringOperation;

const EnumPropertyItem rna_enum_node_match_string_items[] = {
    {NODE_MATCH_STR_STARTS_WITH,
     "STARTS_WITH",
     0,
     "Starts With",
     "True when the first input starts with the second"},
    {NODE_MATCH_STR_ENDS_WITH,
     "ENDS_WITH",
     0,
     "Ends With",
     "True when the first input ends with the second"},
    {NODE_MATCH_STR_CONTAINS,
     "CONTAINS",
     0,
     "Contains",
     "True when the first input contains the second as a substring"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("A").hide_label();
  b.add_input<decl::String>("B").hide_label();
  b.add_output<decl::Bool>("Result");
}

static const mf::MultiFunction *get_multi_function(const bNode &bnode)
{
  const NodeMatchStringOperation operation = NodeMatchStringOperation(bnode.custom1);

  switch (operation) {
    case NODE_MATCH_STR_STARTS_WITH: {
      static auto fn = mf::build::SI2_SO<std::string, std::string, bool>(
          "Starts With",
          [](std::string a, std::string b) { return BLI_str_startswith(a.c_str(), b.c_str()); });
      return &fn;
    }
    case NODE_MATCH_STR_ENDS_WITH: {
      static auto fn = mf::build::SI2_SO<std::string, std::string, bool>(
          "Ends With",
          [](std::string a, std::string b) { return BLI_str_endswith(a.c_str(), b.c_str()); });
      return &fn;
    }
    case NODE_MATCH_STR_CONTAINS: {
      static auto fn = mf::build::SI2_SO<std::string, std::string, bool>(
          "Contains", [](std::string a, std::string b) { return a.find(b) != std::string::npos; });
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

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = NODE_MATCH_STR_STARTS_WITH;
}

static void node_label(const bNodeTree * /*tree*/,
                       const bNode *node,
                       char *label,
                       int label_maxncpy)
{
  const char *name;
  bool enum_label = RNA_enum_name(rna_enum_node_match_string_items, node->custom1, &name);
  if (!enum_label) {
    name = "Unknown";
  }
  BLI_strncpy_utf8(label, IFACE_(name), label_maxncpy);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "operation",
                    "Operation",
                    "",
                    rna_enum_node_match_string_items,
                    NOD_inline_enum_accessors(custom1),
                    NODE_MATCH_STR_STARTS_WITH);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeMatchString");
  ntype.ui_name = "Match String";
  ntype.enum_name_legacy = "MATCH_STRING";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.labelfunc = node_label;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.build_multi_function = node_build_multi_function;

  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_match_string_cc
