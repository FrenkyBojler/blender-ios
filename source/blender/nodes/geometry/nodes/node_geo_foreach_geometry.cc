/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_string_utf8.h"

#include "BLO_read_write.hh"

#include "NOD_geo_foreach_geometry.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_prototypes.hh"

namespace blender::nodes::node_geo_foreach_geometry_cc {

namespace input_node {

NODE_STORAGE_FUNCS(NodeGeometryForeachGeometryInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry");
  b.add_output<decl::Geometry>("Geometry").align_with_previous().propagate_all();
}

static void node_label(const bNodeTree * /*ntree*/,
                       const bNode * /*node*/,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(label, IFACE_("For Each Geometry"), label_maxncpy);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryForeachGeometryInput *data = MEM_callocN<NodeGeometryForeachGeometryInput>(__func__);
  node->storage = data;
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeForeachGeometryInput", GEO_NODE_FOREACH_GEOMETRY_INPUT);
  ntype.ui_name = "For Each Geometry Input";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = nullptr;
  ntype.initfunc = node_init;
  ntype.labelfunc = node_label;
  ntype.no_muting = true;
  bke::node_type_storage(ntype,
                         "NodeGeometryForeachGeometryInput",
                         node_free_standard_storage,
                         node_copy_standard_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace input_node

namespace output_node {

NODE_STORAGE_FUNCS(NodeGeometryForeachGeometryOutput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();

  if (!node || !tree) {
    return;
  }

  const NodeGeometryForeachGeometryOutput &storage = node_storage(*node);
  for (const int i : IndexRange(storage.output_items.items_num)) {
    const NodeGeometryForeachGeometryOutputItem &item = storage.output_items.items[i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const StringRef name = item.name ? item.name : "";
    std::string identifier = ForeachGeometryOutputItemsAccessor::socket_identifier_for_item(item);
    b.add_input(socket_type, name, identifier)
        .socket_name_ptr(&tree->id, ForeachGeometryOutputItemsAccessor::item_srna, &item, "name")
        .description("New geometry where each unique instanced geometry has been replaced");
    b.add_output<decl::Geometry>(name, identifier).align_with_previous().propagate_all();
  }

  b.add_input<decl::Extend>("", "__extend__").structure_type(StructureType::Dynamic);
  b.add_output<decl::Extend>("", "__extend__")
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryForeachGeometryOutput *data = MEM_callocN<NodeGeometryForeachGeometryOutput>(
      __func__);

  data->output_items.items = MEM_calloc_arrayN<NodeGeometryForeachGeometryOutputItem>(1, __func__);
  NodeGeometryForeachGeometryOutputItem &item = data->output_items.items[0];
  item.name = BLI_strdup(DATA_("Geometry"));
  item.socket_type = SOCK_GEOMETRY;
  item.identifier = data->output_items.next_identifier++;
  data->output_items.items_num = 1;

  node->storage = data;
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *current_node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(current_node_ptr->owner_id);
  bNode *current_node = static_cast<bNode *>(current_node_ptr->data);
  bNode *output_node = current_node;

  if (uiLayout *panel = layout->panel(C, "output_items", false, IFACE_("Items"))) {
    socket_items::ui::draw_items_list_with_operators<ForeachGeometryOutputItemsAccessor>(
        C, panel, ntree, *output_node);
    socket_items::ui::draw_active_item_props<ForeachGeometryOutputItemsAccessor>(
        ntree, *output_node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryForeachGeometryOutput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_dupallocN<NodeGeometryForeachGeometryOutput>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<ForeachGeometryOutputItemsAccessor>(*src_node, *dst_node);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ForeachGeometryOutputItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static bool node_insert_link(bNodeTree *ntree, bNode *node, bNodeLink *link)
{
  return socket_items::try_add_item_via_any_extend_socket<ForeachGeometryOutputItemsAccessor>(
      *ntree, *node, *node, *link, "__extend__");
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ForeachGeometryOutputItemsAccessor>();
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<ForeachGeometryOutputItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<ForeachGeometryOutputItemsAccessor>(&reader, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(
      &ntype, "GeometryNodeForeachGeometryOutput", GEO_NODE_FOREACH_GEOMETRY_OUTPUT);
  ntype.ui_name = "For Each Geometry Output";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.labelfunc = input_node::node_label;
  ntype.no_muting = true;
  ntype.insert_link = node_insert_link;
  ntype.gather_link_search_ops = nullptr;
  ntype.register_operators = node_operators;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  bke::node_type_storage(
      ntype, "NodeGeometryForeachGeometryOutput", node_free_storage, node_copy_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace output_node

}  // namespace blender::nodes::node_geo_foreach_geometry_cc

namespace blender::nodes {

StructRNA *ForeachGeometryOutputItemsAccessor::item_srna =
    &RNA_NodeGeometryForeachGeometryOutputItem;

void ForeachGeometryOutputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ForeachGeometryOutputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
