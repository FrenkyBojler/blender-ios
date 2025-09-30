/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_socket_search_link.hh"
#include "node_geometry_util.hh"

#include "BKE_volume_grid.hh"
#include "BKE_volume_grid_fields.hh"
#include "BKE_volume_openvdb.hh"

#include "NOD_geo_field_to_grid.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "../intern/volume_grid_function_eval.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "BLO_read_write.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#endif

namespace blender::nodes::node_geo_field_to_grid_cc {

NODE_STORAGE_FUNCS(NodeFieldToGrid)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const NodeFieldToGrid &storage = node_storage(*node);
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(storage.data_type);

  b.add_input(data_type, "Topology").structure_type(StructureType::Grid);

  const Span<NodeFieldToGridItem> items(storage.items, storage.items_num);
  for (const int i : items.index_range()) {
    const NodeFieldToGridItem &item = items[i];
    const eNodeSocketDatatype data_type = eNodeSocketDatatype(item.data_type);
    const std::string input_identifier =
        FieldToGridItemsAccessor::input_socket_identifier_for_item(item);
    const std::string output_identifier =
        FieldToGridItemsAccessor::output_socket_identifier_for_item(item);

    b.add_input(data_type, item.name, input_identifier).supports_field();
    b.add_output(data_type, item.name, output_identifier)
        .structure_type(StructureType::Grid)
        .align_with_previous()
        .description("Output grid with evaluated field values");
  }

  b.add_input<decl::Extend>("", "__extend__").structure_type(StructureType::Field);
  b.add_output<decl::Extend>("", "__extend__")
      .structure_type(StructureType::Grid)
      .align_with_previous();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *static_cast<bNode *>(ptr->data);
  if (uiLayout *panel = layout->panel(C, "field_to_grid_items", false, IFACE_("Fields"))) {
    socket_items::ui::draw_items_list_with_operators<FieldToGridItemsAccessor>(
        C, panel, tree, node);
    socket_items::ui::draw_active_item_props<FieldToGridItemsAccessor>(
        tree, node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "data_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static std::optional<eNodeSocketDatatype> node_type_for_socket_type(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_FLOAT:
      return SOCK_FLOAT;
    case SOCK_BOOLEAN:
      return SOCK_BOOLEAN;
    case SOCK_INT:
      return SOCK_INT;
    case SOCK_VECTOR:
    case SOCK_RGBA:
      return SOCK_VECTOR;
    default:
      return std::nullopt;
  }
}

static void node_gather_link_search_ops(GatherLinkSearchOpParams &params)
{
  if (!USER_EXPERIMENTAL_TEST(&U, use_new_volume_nodes)) {
    return;
  }
  const std::optional<eNodeSocketDatatype> data_type = node_type_for_socket_type(
      params.other_socket());
  if (!data_type) {
    return;
  }
  if (params.in_out() == SOCK_IN) {
    params.add_item(IFACE_("Field"), [data_type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeFieldToGrid");
      socket_items::add_item_with_socket_type_and_name<FieldToGridItemsAccessor>(
          params.node_tree, node, *data_type, params.socket.name);
      params.update_and_connect_available_socket(node, params.socket.name);
    });
  }
  else {
    params.add_item(IFACE_("Grid"), [data_type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeFieldToGrid");
      socket_items::add_item_with_socket_type_and_name<FieldToGridItemsAccessor>(
          params.node_tree, node, *data_type, params.socket.name);
      params.update_and_connect_available_socket(node, params.socket.name);
    });
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const NodeFieldToGrid &storage = node_storage(params.node());
  const Span<NodeFieldToGridItem> items(storage.items, storage.items_num);
  params.set_default_remaining_outputs();
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_init(bNodeTree *tree, bNode *node)
{
  NodeFieldToGrid *data = MEM_callocN<NodeFieldToGrid>(__func__);
  data->data_type = SOCK_FLOAT;
  node->storage = data;
  socket_items::add_item_with_socket_type_and_name<FieldToGridItemsAccessor>(
      *tree, *node, SOCK_FLOAT, "Value");
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<FieldToGridItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeFieldToGrid &src_storage = node_storage(*src_node);
  NodeFieldToGrid *dst_storage = MEM_dupallocN<NodeFieldToGrid>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<FieldToGridItemsAccessor>(*src_node, *dst_node);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<FieldToGridItemsAccessor>();
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  return socket_items::try_add_item_via_any_extend_socket<FieldToGridItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<FieldToGridItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<FieldToGridItemsAccessor>(&reader, node);
}

static const bNodeSocket *node_internally_linked_input(const bNodeTree & /*tree*/,
                                                       const bNode &node,
                                                       const bNodeSocket &output_socket)
{
  return node.input_by_identifier(output_socket.identifier);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeFieldToGrid");
  ntype.ui_name = "Field to Grid";
  ntype.ui_description =
      "Create new grids by evaluating new values on an existing volume grid topology";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype, "NodeFieldToGrid", node_free_storage, node_copy_storage);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.register_operators = node_operators;
  ntype.insert_link = node_insert_link;
  ntype.ignore_inferred_input_socket_visibility = true;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.internally_linked_input = node_internally_linked_input;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_field_to_grid_cc

namespace blender::nodes {

StructRNA *FieldToGridItemsAccessor::item_srna = &RNA_NodeFieldToGridItem;

void FieldToGridItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void FieldToGridItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
