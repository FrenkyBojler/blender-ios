/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_closure_eval.hh"
#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_closure_to_list_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.add_input<decl::Int>("Count").default_value(1).min(1).description(
      "The number of elements in the list");

  if (node != nullptr) {
    // const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input<decl::Closure>("Value");
  }

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_output(type, "List").structure_type(StructureType::List);
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

class SocketSearchOp {
 public:
  const StringRef socket_name;
  eNodeSocketDatatype socket_type;
  void operator()(LinkSearchOpParams &params)
  {
    bNode &node = params.add_node("GeometryNodeClosureToList");
    node.custom1 = socket_type;
    params.update_and_connect_available_socket(node, socket_name);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  if (!U.experimental.use_geometry_nodes_lists) {
    return;
  }
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN) {
    if (params.node_tree().typeinfo->validate_link(socket_type, SOCK_INT)) {
      params.add_item(IFACE_("Count"), SocketSearchOp{"Count", SOCK_INT});
    }
    if (params.node_tree().typeinfo->validate_link(socket_type, SOCK_CLOSURE)) {
      params.add_item(IFACE_("Value"), SocketSearchOp{"Value", SOCK_CLOSURE});
    }
  }
  else {
    params.add_item(IFACE_("List"), SocketSearchOp{"List", socket_type});
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const int count = params.extract_input<int>("Count");
  if (count < 0) {
    params.error_message_add(NodeWarningType::Error, "Count must not be negative");
    params.set_default_remaining_outputs();
    return;
  }
  const auto socket_type = eNodeSocketDatatype(params.node().custom1);
  ClosurePtr closure = params.extract_input<ClosurePtr>("Value");

  const CPPType *cpp_type = bke::socket_type_to_geo_nodes_base_cpp_type(socket_type);
  List::ArrayData array_data = List::ArrayData::ForUninitialized(*cpp_type, count);
  GMutableSpan values(*cpp_type, array_data.data, count);

  const bke::bNodeSocketType *int_type = bke::node_socket_type_find("NodeSocketInt");
  const bke::bNodeSocketType *socket_type_ptr = bke::node_socket_type_find_static(socket_type);

  /* The grain size is completely arbitrary since we don't know how expensive the closure is.
   * However since the closure evaluation itself has fairly high overhead, it makes to optimize for
   * the case where each task has a relatively high cost. */
  threading::parallel_for(IndexRange(count), 8, [&](const IndexRange range) {
    ClosureEagerEvalParams closure_params;
    closure_params.user_data = params.user_data();
    closure_params.inputs.append({"index", int_type, bke::SocketValueVariant::From(0)});
    bke::SocketValueVariant value;
    closure_params.outputs.append({"value", socket_type_ptr, &value});

    for (const int64_t i : range) {
      BLI_assert(i < std::numeric_limits<int>::max());
      *static_cast<int *>(
          const_cast<void *>(closure_params.inputs[0].value.get_single_ptr_raw())) = int(i);

      value.~SocketValueVariant();
      evaluate_closure_eagerly(*closure, closure_params);

      cpp_type->move_construct(const_cast<void *>(value.get_single_ptr_raw()), values[i]);
    }
  });

  ListPtr list = List::create(*cpp_type, std::move(array_data), count);
  params.set_output("List", std::move(list));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_inline_enum_accessors(custom1),
      SOCK_GEOMETRY,
      [](bContext * /*C*/, PointerRNA *ptr, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        const bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
        blender::bke::bNodeTreeType *ntree_type = ntree.typeinfo;
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [&](const EnumPropertyItem &item) -> bool {
              bke::bNodeSocketType *socket_type = bke::node_socket_type_find_static(item.value);
              return ntree_type->valid_socket_type(ntree_type, socket_type);
            });
      });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeClosureToList");
  ntype.ui_name = "Closure to List";
  ntype.ui_description = "Create a list of values";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_closure_to_list_cc
