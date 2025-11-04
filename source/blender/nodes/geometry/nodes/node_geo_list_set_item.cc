/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "BLI_bit_vector.hh"

#include "list_function_eval.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_list_set_item_cc {

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

  b.add_input<decl::Int>("Index")
      .default_value(0)
      .structure_type(StructureType::Dynamic)
      .description("Index or indices of elements to replace (negative counts from end)");

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "Value")
        .field_on_all()
        .description("New value for the element (can be a field evaluated at each index)");
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
    bNode &node = params.add_node("GeometryNodeListSetItem");
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
      params.add_item(IFACE_("Index"), SocketSearchOp{"Index", SOCK_INT});
    }
    params.add_item(IFACE_("List"), SocketSearchOp{"List", socket_type});
    params.add_item(IFACE_("Value"), SocketSearchOp{"Value", socket_type});
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
    params.error_message_add(NodeWarningType::Warning, "Cannot set item in empty list");
    params.set_output("List", std::move(list));
    return;
  }

  const CPPType &type = list->cpp_type();

  bke::SocketValueVariant index_variant = params.extract_input<bke::SocketValueVariant>("Index");

  Vector<int> indices;
  if (index_variant.is_single()) {
    int index = index_variant.get<int>();
    if (index < 0) {
      index = list_size + index;
    }
    index = std::clamp(index, 0, list_size - 1);
    indices.append(index);
  }
  else if (index_variant.is_list()) {
    ListPtr index_list = index_variant.get<ListPtr>();
    const VArray<int> index_varray = index_list->varray<int>();
    for (int i = 0; i < index_list->size(); i++) {
      int index = index_varray[i];
      if (index < 0) {
        index = list_size + index;
      }
      index = std::clamp(index, 0, list_size - 1);
      indices.append(index);
    }
  }
  else {
    params.error_message_add(NodeWarningType::Error, "Invalid index input");
    params.set_default_remaining_outputs();
    return;
  }

  if (indices.is_empty()) {
    params.set_output("List", std::move(list));
    return;
  }

  bke::SocketValueVariant value_variant = params.extract_input<bke::SocketValueVariant>("Value");

  ListPtr value_list;
  if (value_variant.is_context_dependent_field()) {
    GField field = value_variant.extract<GField>();
    value_list = evaluate_field_to_list(std::move(field), list_size);
  }
  else {
    value_variant.convert_to_single();
    const void *single_value = value_variant.get_single_ptr_raw();
    List::SingleData single_data = List::SingleData::ForValue(GPointer(type, single_value));
    value_list = List::create(type, std::move(single_data), list_size);
  }

  List::ArrayData result_data = List::ArrayData::ForUninitialized(type, list_size);
  GMutableSpan dst_span(type, result_data.data, list_size);

  const GVArray src_varray = list->varray();
  for (int i = 0; i < list_size; i++) {
    src_varray.get_to_uninitialized(i, dst_span[i]);
  }

  const GVArray value_varray = value_list->varray();
  /* Use a visited bitset to avoid double-destruction when indices contains duplicates. */
  BitVector<> visited(list_size, false);
  for (const int index : indices) {
    /* Skip if this index has already been processed to avoid double-destruction. */
    if (visited[index]) {
      continue;
    }
    type.destruct(dst_span[index]);
    value_varray.get_to_uninitialized(index, dst_span[index]);
    visited[index].set();
  }

  ListPtr result_list = List::create(type, std::move(result_data), list_size);
  params.set_output("List", std::move(result_list));
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
  geo_node_type_base(&ntype, "GeometryNodeListSetItem");
  ntype.ui_name = "Set List Item";
  ntype.ui_description =
      "Replace values at specific indices in a list (supports field inputs and multiple indices)";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_set_item_cc
