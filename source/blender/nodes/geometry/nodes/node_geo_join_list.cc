/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_join_list_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "Value").multi_input().hide_value().structure_type(StructureType::Dynamic);
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
    bNode &node = params.add_node("GeometryNodeJoinList");
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
  const auto socket_type = eNodeSocketDatatype(params.node().custom1);
  auto inputs = params.extract_input<GeoNodesMultiInput<bke::SocketValueVariant>>("Value");

  Array<int, 16> size_offset_data(inputs.values.size() + 1);
  for (const int i : inputs.values.index_range()) {
    if (inputs.values[i].is_list()) {
      size_offset_data[i] = inputs.values[i].get<ListPtr>()->size();
    }
    else {
      size_offset_data[i] = 1;
    }
  }

  const OffsetIndices offsets = offset_indices::accumulate_counts_to_offsets(size_offset_data);
  const int64_t size = offsets.total_size();

  const CPPType *cpp_type = bke::socket_type_to_geo_nodes_base_cpp_type(socket_type);
  List::ArrayData array_data = List::ArrayData::ForUninitialized(*cpp_type, size);
  GMutableSpan dst_list_data(*cpp_type, array_data.data, size);

  for (const int i : inputs.values.index_range()) {
    GMutableSpan dst = dst_list_data.slice(offsets[i]);
    if (inputs.values[i].is_list()) {
      const ListPtr &list = inputs.values[i].get<ListPtr>();

      /* Move the input values if both the input list and its data aren't shared. */
      if (list->is_mutable()) {
        if (const auto *array_data = std::get_if<List::ArrayData>(&list->data())) {
          if (array_data->sharing_info->is_mutable()) {
            cpp_type->move_construct_n(array_data->data, dst.data(), dst.size());
            continue;
          }
        }
      }

      list->varray().materialize_to_uninitialized(dst.data());
    }
    else {
      GMutablePointer src = inputs.values[i].get_single_ptr();
      cpp_type->move_construct(src.get(), dst.data());
    }
  }

  ListPtr list = List::create(*cpp_type, std::move(array_data), size);
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
  geo_node_type_base(&ntype, "GeometryNodeJoinList");
  ntype.ui_name = "Join List";
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

}  // namespace blender::nodes::node_geo_join_list_cc
