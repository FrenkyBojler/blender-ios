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

namespace blender::nodes::node_geo_list_slice_cc {

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

  b.add_input<decl::Int>("Start").default_value(0).description("Starting index (inclusive)");
  b.add_input<decl::Int>("End").default_value(-1).description(
      "Ending index (exclusive, -1 means end of list)");
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
    bNode &node = params.add_node("GeometryNodeListSlice");
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
      params.add_item(IFACE_("Start"), SocketSearchOp{"Start", SOCK_INT});
      params.add_item(IFACE_("End"), SocketSearchOp{"End", SOCK_INT});
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
  int start = params.extract_input<int>("Start");
  int end = params.extract_input<int>("End");

  /* Handle negative indices Python-style. */
  if (start < 0) {
    start = list_size + start;
  }
  if (end < 0) {
    end = list_size + end + 1;
  }

  /* Clamp to valid range. */
  start = std::clamp(start, 0, list_size);
  end = std::clamp(end, 0, list_size);

  /* Ensure start <= end. */
  if (start >= end) {
    /* Return empty list. */
    const CPPType &type = list->cpp_type();
    List::ArrayData empty_data = List::ArrayData::ForDefaultValue(type, 0);
    ListPtr empty_list = List::create(type, std::move(empty_data), 0);
    params.set_output("List", std::move(empty_list));
    return;
  }

  const int slice_size = end - start;

  /* If slicing the entire list, return as-is. */
  if (start == 0 && end == list_size) {
    params.set_output("List", std::move(list));
    return;
  }

  const CPPType &type = list->cpp_type();
  const List::DataVariant &list_data = list->data();

  if (const auto *single_data = std::get_if<List::SingleData>(&list_data)) {
    /* For single data, create a list with the same value. */
    List::SingleData slice_data = List::SingleData::ForValue(GPointer(type, single_data->value));
    ListPtr sliced_list = List::create(type, std::move(slice_data), slice_size);
    params.set_output("List", std::move(sliced_list));
    return;
  }

  if (const auto *array_data = std::get_if<List::ArrayData>(&list_data)) {
    const GSpan src_span(type, array_data->data, list_size);
    List::ArrayData slice_data = List::ArrayData::ForUninitialized(type, slice_size);
    GMutableSpan dst_span(type, slice_data.data, slice_size);

    for (int i = 0; i < slice_size; i++) {
      type.copy_construct(src_span[start + i], dst_span[i]);
    }

    ListPtr sliced_list = List::create(type, std::move(slice_data), slice_size);
    params.set_output("List", std::move(sliced_list));
  }
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
  geo_node_type_base(&ntype, "GeometryNodeListSlice");
  ntype.ui_name = "List Slice";
  ntype.ui_description = "Extract a portion of a list";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_slice_cc
