/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bit_vector.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_geometry_nodes_values.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "list_function_eval.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_list_set_item_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  if (!node) {
    return;
  }

  const auto type = eNodeSocketDatatype(node->custom1);
  b.add_input(type, "List"_ustr).structure_type(StructureType::List).hide_value();
  b.add_output(type, "List"_ustr)
      .propagate_all({0})
      .structure_type(StructureType::List)
      .align_with_previous();

  b.add_input(type, "Value"_ustr)
      .structure_type(StructureType::Dynamic)
      .description("Can be a single value, field to be evaluated or list of values.");

  b.add_input<decl::Int>("Index"_ustr)
      .default_value(0)
      .structure_type(StructureType::Dynamic)
      .description(
          "Indices of the values to replace. Can be a single value, field, or list of indices.");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "socket_type", UI_ITEM_NONE, "", ICON_NONE);
}

class SocketSearchOp {
 public:
  UString socket_name;
  eNodeSocketDatatype socket_type;
  void operator()(LinkSearchOpParams &params)
  {
    bNode &node = params.add_node("GeometryNodeListSetItem"_ustr);
    node.custom1 = socket_type;
    params.update_and_connect_available_socket(node, socket_name);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN) {
    if (params.node_tree().typeinfo->validate_link(socket_type, SOCK_INT)) {
      params.add_item(IFACE_("Index"), SocketSearchOp{"Index"_ustr, SOCK_INT});
    }
    params.add_item(IFACE_("List"), SocketSearchOp{"List"_ustr, socket_type});
    params.add_item(IFACE_("Value"), SocketSearchOp{"Value"_ustr, socket_type});
  }
  else {
    params.add_item(IFACE_("List"), SocketSearchOp{"List"_ustr, socket_type});
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GListPtr list = params.extract_input<GListPtr>("List"_ustr);

  if (!list) {
    params.set_default_remaining_outputs();
    return;
  }

  if (!params.output_is_required("List"_ustr)) {
    return;
  }

  const int list_size = list->size();

  if (list_size == 0) {
    params.error_message_add(NodeWarningType::Warning, "Cannot set item in empty list");
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  const CPPType &type = list->cpp_type();

  bke::SocketValueVariant index_variant = params.extract_input<bke::SocketValueVariant>(
      "Index"_ustr);

  Vector<int> indices;
  if (index_variant.is_single()) {
    int index = index_variant.get<int>();
    if (index < 0) {
      index = list_size + index;
    }
    index = std::clamp(index, 0, list_size - 1);
    indices.append(index);
  }
  else if (index_variant.is_context_dependent_field()) {
    const GListPtr index_list = evaluate_field_to_list(index_variant.extract<GField>(), list_size);
    const VArray<int> index_varray = index_list->varray().typed<int>();
    for (int i = 0; i < index_list->size(); i++) {
      int index = index_varray[i];
      if (index < 0) {
        index = list_size + index;
      }
      index = std::clamp(index, 0, list_size - 1);
      indices.append(index);
    }
  }
  else if (index_variant.is_list()) {
    GListPtr index_list = index_variant.get<GListPtr>();
    if (!index_list) {
      params.set_output("List"_ustr, std::move(list));
      return;
    }
    const VArray<int> index_varray = index_list->varray().typed<int>();
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
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  bke::SocketValueVariant value_variant = params.extract_input<bke::SocketValueVariant>(
      "Value"_ustr);

  GListPtr value_list;
  if (value_variant.is_context_dependent_field()) {
    value_list = evaluate_field_to_list(value_variant.extract<GField>(), list->size());
  }
  else if (value_variant.is_list()) {
    value_list = value_variant.get<GListPtr>();
    if (!value_list) {
      params.set_output("List"_ustr, std::move(list));
      return;
    }
    if (value_list->size() != list_size) {
      params.error_message_add(NodeWarningType::Error,
                               "\"Value\" list size must match \"List\" size");
      params.set_default_remaining_outputs();
      return;
    }
  }
  else {
    value_variant.convert_to_single();
    const void *single_value = value_variant.get_single_ptr_raw();
    GList::SingleData single_data = GList::SingleData::ForValue(GPointer(type, single_value));
    value_list = GList::create(type, std::move(single_data), list_size);
  }

  GList::ArrayData result_data = GList::ArrayData::ForUninitialized(type, list_size);
  GMutableSpan dst_span = result_data.span_for_write(type, list_size);

  const GVArray src_varray = list->varray();
  for (int i = 0; i < list_size; i++) {
    src_varray.get_to_uninitialized(i, dst_span[i]);
  }

  const GVArray value_varray = value_list->varray();
  /* Use a visited bitset to avoid double-destruction when indices contains duplicates. */
  BitVector<> visited(list_size, false);
  for (const int index : indices) {
    if (visited[index]) {
      continue;
    }
    type.destruct(dst_span[index]);
    value_varray.get_to_uninitialized(index, dst_span[index]);
    visited[index].set();
  }

  GListPtr result_list = GList::create(type, std::move(result_data), list_size);
  params.set_output("List"_ustr, std::move(result_list));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "socket_type",
      "Socket Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_inline_enum_accessors(custom1),
      SOCK_FLOAT,
      [](bContext * /*C*/, PointerRNA *ptr, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        const bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
        bke::bNodeTreeType *ntree_type = ntree.typeinfo;
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [&](const EnumPropertyItem &item) -> bool {
              bke::bNodeSocketType *socket_type = bke::node_socket_type_find_static(item.value);
              return ntree_type->valid_socket_type(ntree_type, socket_type);
            });
      });
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeListSetItem"_ustr);
  ntype.ui_name = "Set List Item";
  ntype.ui_description = "Replace values at specific indices in a list";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_set_item_cc
