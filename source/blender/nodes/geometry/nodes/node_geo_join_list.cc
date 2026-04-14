/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_join_list_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "List"_ustr)
        .structure_type(StructureType::List)
        .multi_input()
        .hide_value()
        .description("Lists to join together");
  }

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_output(type, "List"_ustr).structure_type(StructureType::List).align_with_previous();
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

class SocketSearchOp {
 public:
  eNodeSocketDatatype socket_type;
  void operator()(LinkSearchOpParams &params)
  {
    bNode &node = params.add_node("GeometryNodeJoinList"_ustr);
    node.custom1 = socket_type;
    params.update_and_connect_available_socket(node, "List"_ustr);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  if (!U.experimental.use_geometry_nodes_lists) {
    return;
  }
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(params.other_socket().type);
  params.add_item(IFACE_("List"), SocketSearchOp{socket_type});
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeoNodesMultiInput<ListPtr> lists = params.extract_input<GeoNodesMultiInput<ListPtr>>("List"_ustr);

  if (lists.values.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  if (!params.output_is_required("List"_ustr)) {
    return;
  }

  Vector<ListPtr> valid_lists;
  const CPPType *common_type = nullptr;

  for (ListPtr &list : lists.values) {
    if (list) {
      if (common_type == nullptr) {
        common_type = &list->cpp_type();
      }
      else if (common_type != &list->cpp_type()) {
        params.error_message_add(NodeWarningType::Error, "All lists must have the same data type");
        params.set_default_remaining_outputs();
        return;
      }
      valid_lists.append(std::move(list));
    }
  }

  if (valid_lists.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  if (valid_lists.size() == 1) {
    params.set_output("List"_ustr, std::move(valid_lists[0]));
    return;
  }

  int64_t total_size = 0;
  for (const ListPtr &list : valid_lists) {
    total_size += list->size();
  }

  if (total_size == 0) {
    List::ArrayData empty_data = List::ArrayData::ForDefaultValue(*common_type, 0);
    ListPtr empty_list = List::create(*common_type, std::move(empty_data), 0);
    params.set_output("List"_ustr, std::move(empty_list));
    return;
  }

  /* Check if all are single data with the same value. */
  bool all_single = true;
  const void *first_value = nullptr;
  for (const ListPtr &list : valid_lists) {
    if (const auto *single_data = std::get_if<List::SingleData>(&list->data())) {
      if (first_value == nullptr) {
        first_value = single_data->value;
      }
      else if (!common_type->is_equal(first_value, single_data->value)) {
        all_single = false;
        break;
      }
    }
    else {
      all_single = false;
      break;
    }
  }

  if (all_single && first_value != nullptr) {
    List::SingleData joined_data = List::SingleData::ForValue(GPointer(*common_type, first_value));
    ListPtr joined_list = List::create(*common_type, std::move(joined_data), total_size);
    params.set_output("List"_ustr, std::move(joined_list));
    return;
  }

  List::ArrayData joined_data = List::ArrayData::ForUninitialized(*common_type, total_size);
  GMutableSpan dst_span = joined_data.span_for_write(*common_type, total_size);

  int64_t offset = 0;
  for (const ListPtr &list : valid_lists) {
    const int64_t list_size = list->size();
    const GVArray varray = list->varray();
    for (int64_t i = 0; i < list_size; i++) {
      varray.get_to_uninitialized(i, dst_span[offset + i]);
    }
    offset += list_size;
  }

  ListPtr joined_list = List::create(*common_type, std::move(joined_data), total_size);
  params.set_output("List"_ustr, std::move(joined_list));
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
  geo_node_type_base(&ntype, "GeometryNodeJoinList"_ustr);
  ntype.ui_name = "Join List";
  ntype.ui_description = "Join multiple lists together";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_join_list_cc
