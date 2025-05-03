/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string.h"

#include "RNA_enum_types.hh"

#include "UI_interface.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "node_function_util.hh"

static_assert(-1 == ~0, "Two's complement must be used for bitwise operations.");

namespace blender::nodes::node_fn_bit_math_cc {

constexpr static int32_t max_shift = sizeof(int32_t) * CHAR_BIT - 1;
constexpr static int32_t min_shift = -max_shift;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Int>("Value");
  auto &value2 = b.add_input<decl::Int>("Value", "Value_001");
  auto &shift = b.add_input<decl::Int>("Shift").min(-max_shift).max(max_shift);
  b.add_output<decl::Int>("Value");

  if (const bNode *node = b.node_or_null()) {
    const NodeBitMathOperation operation = NodeBitMathOperation(node->custom1);
    value2.available(
        !ELEM(operation, NODE_BIT_MATH_NOT, NODE_BIT_MATH_SHIFT, NODE_BIT_MATH_ROTATE));
    shift.available(ELEM(operation, NODE_BIT_MATH_SHIFT, NODE_BIT_MATH_ROTATE));
  }
};

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "operation", UI_ITEM_NONE, "", ICON_NONE);
}

class SocketSearchOp {
 public:
  std::string socket_name;
  NodeBitMathOperation operation;
  void operator()(LinkSearchOpParams &params)
  {
    bNode &node = params.add_node("FunctionNodeBitMath");
    node.custom1 = NodeBitMathOperation(operation);
    params.update_and_connect_available_socket(node, socket_name);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  if (!params.node_tree().typeinfo->validate_link(eNodeSocketDatatype(params.other_socket().type),
                                                  SOCK_INT))
  {
    return;
  }

  const bool is_integer = params.other_socket().type == SOCK_INT;
  const int weight = is_integer ? 0 : -1;

  /* Add socket A operations. */
  for (const auto *item = rna_enum_node_bit_math_items; item->identifier != nullptr; item++) {
    if (item->name != nullptr && item->identifier[0] != '\0') {
      params.add_item(
          IFACE_(item->name), SocketSearchOp{"Value", NodeBitMathOperation(item->value)}, weight);
    }
  }
}

static void node_label(const bNodeTree * /*ntree*/, const bNode *node, char *label, int maxlen)
{
  const char *name;
  const bool enum_label = RNA_enum_name(rna_enum_node_bit_math_items, node->custom1, &name);
  if (!enum_label) {
    name = "Unknown";
  }
  BLI_strncpy(label, IFACE_(name), maxlen);
}

static inline uint32_t rotate_left(uint32_t n, uint32_t c)
{
  const uint32_t mask = CHAR_BIT * sizeof(n) - 1;
  c &= mask;
  return (n << c) | (n >> ((-c) & mask));
}

static inline uint32_t rotate_right(uint32_t n, uint32_t c)
{
  const uint32_t mask = CHAR_BIT * sizeof(n) - 1;
  c &= mask;
  return (n >> c) | (n << ((-c) & mask));
}

static const mf::MultiFunction *get_multi_function(const bNode &bnode)
{
  NodeBitMathOperation operation = NodeBitMathOperation(bnode.custom1);
  static auto exec_preset = mf::build::exec_presets::AllSpanOrSingle();
  static auto and_fn = mf::build::SI2_SO<int, int, int>(
      "And", [](int a, int b) { return a & b; }, exec_preset);
  static auto or_fn = mf::build::SI2_SO<int, int, int>(
      "Or", [](int a, int b) { return a | b; }, exec_preset);
  static auto xor_fn = mf::build::SI2_SO<int, int, int>(
      "Xor", [](int a, int b) { return a ^ b; }, exec_preset);
  static auto not_fn = mf::build::SI1_SO<int, int>(
      "Not", [](int a) { return ~a; }, exec_preset);
  static auto shift_fn = mf::build::SI2_SO<int, int, int>(
      "Shift",
      [](int a, int b) {
        if (math::abs(b) > max_shift) {
          return 0;
        }
        const int32_t shift = math::abs(b) % (sizeof(uint32_t) * CHAR_BIT);
        uint32_t u = *reinterpret_cast<uint32_t *>(&a);
        u = b >= 0 ? (u << shift) : (u >> shift);
        return *reinterpret_cast<int *>(&u);
      },
      exec_preset);
  static auto rotate_fn = mf::build::SI2_SO<int, int, int>(
      "Rotate",
      [](int a, int b) {
        const uint32_t shift = math::abs(b) % (sizeof(uint32_t) * CHAR_BIT);
        uint32_t u = *reinterpret_cast<uint32_t *>(&a);
        u = b >= 0 ? rotate_left(u, shift) : rotate_right(u, shift);
        return *reinterpret_cast<int *>(&u);
      },
      exec_preset);

  switch (operation) {
    case NODE_BIT_MATH_AND:
      return &and_fn;
    case NODE_BIT_MATH_OR:
      return &or_fn;
    case NODE_BIT_MATH_XOR:
      return &xor_fn;
    case NODE_BIT_MATH_NOT:
      return &not_fn;
    case NODE_BIT_MATH_SHIFT:
      return &shift_fn;
    case NODE_BIT_MATH_ROTATE:
      return &rotate_fn;
  }
  BLI_assert_unreachable();
  return nullptr;
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const mf::MultiFunction *fn = get_multi_function(builder.node());
  builder.set_matching_fn(fn);
}

static void node_rna(StructRNA *srna)
{
  PropertyRNA *prop;

  prop = RNA_def_node_enum(srna,
                           "operation",
                           "Operation",
                           "",
                           rna_enum_node_bit_math_items,
                           NOD_inline_enum_accessors(custom1),
                           NODE_BIT_MATH_AND);
  RNA_def_property_update_runtime(prop, rna_Node_socket_update);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeBitMath");
  ntype.ui_name = "Bit Math";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.labelfunc = node_label;
  ntype.build_multi_function = node_build_multi_function;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;

  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_bit_math_cc
