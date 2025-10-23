/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value.hh"

#include "BLI_sort.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "list_function_eval.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_list_sort_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "List").structure_type(StructureType::List).hide_value();
    b.add_output(type, "List").structure_type(StructureType::List).align_with_previous();
  }

  b.add_input<decl::Float>("Weights")
      .default_value(0.0f)
      .structure_type(StructureType::Dynamic)
      .description("Weights determining the sorted order (can be a single value, list, or field)");
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
    bNode &node = params.add_node("GeometryNodeSortList");
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
    if (params.node_tree().typeinfo->validate_link(socket_type, SOCK_FLOAT)) {
      params.add_item(IFACE_("Weights"), SocketSearchOp{"Weights", SOCK_FLOAT});
    }
    params.add_item(IFACE_("List"), SocketSearchOp{"List", socket_type});
  }
  else {
    params.add_item(IFACE_("List"), SocketSearchOp{"List", socket_type});
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  ListPtr list = params.extract_input<ListPtr>("List");

  if (!list) {
    params.set_default_remaining_outputs();
    return;
  }

  if (!params.output_is_required("List")) {
    return;
  }

  const int list_size = list->size();

  if (list_size == 0) {
    params.set_output("List", std::move(list));
    return;
  }

  /* Extract weights (can be single, list, or field). */
  bke::SocketValueVariant weights_variant = params.extract_input<bke::SocketValueVariant>(
      "Weights");

  ListPtr weights_list;
  if (weights_variant.is_context_dependent_field()) {
    /* Evaluate field for the list size. */
    fn::GField field = weights_variant.extract<fn::GField>();
    weights_list = evaluate_field_to_list(std::move(field), list_size);
  }
  else if (weights_variant.is_list()) {
    /* Direct list input. */
    weights_list = weights_variant.get<ListPtr>();
    if (!weights_list) {
      /* Empty weights, return list unchanged. */
      params.set_output("List", std::move(list));
      return;
    }
    if (weights_list->size() != list_size) {
      params.error_message_add(
          NodeWarningType::Error,
          "List and Weights must have the same length (List: " + std::to_string(list_size) +
              ", Weights: " + std::to_string(weights_list->size()) + ")");
      params.set_default_remaining_outputs();
      return;
    }
  }
  else if (weights_variant.is_single()) {
    /* Single weight value - all items get the same weight, so no sorting needed. */
    params.set_output("List", std::move(list));
    return;
  }
  else {
    /* No weights provided, return unchanged. */
    params.set_output("List", std::move(list));
    return;
  }

  /* Extract weights into a span for sorting. */
  Array<float> weights(list_size);
  const VArray<float> weights_varray = weights_list->varray<float>();
  for (int i = 0; i < list_size; i++) {
    weights[i] = weights_varray[i];
  }

  /* Create indices array for sorting. */
  Array<int> indices(list_size);
  for (int i : indices.index_range()) {
    indices[i] = i;
  }

  /* Sort indices based on weights. */
  const auto comparator = [&](const int index_a, const int index_b) {
    const float weight_a = weights[index_a];
    const float weight_b = weights[index_b];
    if (UNLIKELY(weight_a == weight_b)) {
      /* Make it stable. */
      return index_a < index_b;
    }
    return weight_a < weight_b;
  };

  parallel_sort(indices.begin(), indices.end(), comparator);

  /* Create sorted list by gathering elements in sorted order. */
  const CPPType &type = list->cpp_type();
  const List::DataVariant &list_data = list->data();

  if (std::get_if<List::SingleData>(&list_data)) {
    /* If all values are the same, just return the same list. */
    params.set_output("List", std::move(list));
    return;
  }

  /* Create array data for sorted list. */
  List::ArrayData sorted_array_data = List::ArrayData::ForUninitialized(type, list_size);

  if (const auto *array_data = std::get_if<List::ArrayData>(&list_data)) {
    const GSpan src_span(type, array_data->data, list_size);
    GMutableSpan dst_span(type, sorted_array_data.data, list_size);

    for (const int i : indices.index_range()) {
      const int src_index = indices[i];
      type.copy_construct(src_span[src_index], dst_span[i]);
    }
  }

  ListPtr sorted_list = List::create(type, std::move(sorted_array_data), list_size);
  params.set_output("List", std::move(sorted_list));
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
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [](const EnumPropertyItem &item) -> bool {
              return socket_type_supports_fields(eNodeSocketDatatype(item.value));
            });
      });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeListSort");
  ntype.ui_name = "Sort List";
  ntype.ui_description = "Sort a list based on weights";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_sort_cc
