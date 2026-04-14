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

namespace blender::nodes::node_geo_list_reverse_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (node == nullptr) {
    return;
  }
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
  b.add_input(type, "List"_ustr).structure_type(StructureType::List).hide_value();
  b.add_output(type, "List"_ustr).structure_type(StructureType::List).align_with_previous();
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
    bNode &node = params.add_node("GeometryNodeListReverse"_ustr);
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
  ListPtr list = params.extract_input<ListPtr>("List"_ustr);

  if (!list) {
    params.set_default_remaining_outputs();
    return;
  }

  if (!params.output_is_required("List"_ustr)) {
    return;
  }

  const int list_size = list->size();

  if (list_size <= 1) {
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  const CPPType &type = list->cpp_type();
  const List::DataVariant &list_data = list->data();

  if (std::get_if<List::SingleData>(&list_data)) {
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  if (const auto *array_data = std::get_if<List::ArrayData>(&list_data)) {
    const GSpan src_span(type, array_data->data, list_size);
    List::ArrayData reversed_data = List::ArrayData::ForUninitialized(type, list_size);
    GMutableSpan dst_span = reversed_data.span_for_write(type, list_size);

    for (int i = 0; i < list_size; i++) {
      type.copy_construct(src_span[list_size - 1 - i], dst_span[i]);
    }

    ListPtr reversed_list = List::create(type, std::move(reversed_data), list_size);
    params.set_output("List"_ustr, std::move(reversed_list));
    return;
  }

  params.set_default_remaining_outputs();
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
  geo_node_type_base(&ntype, "GeometryNodeListReverse"_ustr);
  ntype.ui_name = "Reverse List";
  ntype.ui_description = "Reverse the order of elements in a list";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_reverse_cc
