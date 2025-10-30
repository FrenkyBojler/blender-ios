/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>
#include <sstream>

#include <fmt/format.h>

#include "BLI_dot_export.hh"

#include "BLO_read_write.hh"

#include "NOD_expression_parse.hh"
#include "NOD_geo_expression.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "RNA_prototypes.hh"

#include "node_geometry_util.hh"
#include "shader/node_shader_util.hh"

namespace blender::nodes::node_geo_expression_cc {

NODE_STORAGE_FUNCS(NodeExpression)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (!node || !tree) {
    return;
  }
  const NodeExpression &storage = node_storage(*node);

  for (const int i : IndexRange(storage.expression_items.items_num)) {
    const NodeExpressionItem &item = storage.expression_items.items[i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const std::string identifier = ExpressionItemsAccessor::socket_identifier_for_item(item);
    b.add_input<decl::String>(item.name, identifier)
        .optional_label()
        .hide_socket_icon(tree->type == NTREE_COMPOSIT)
        .description("Expression to be evaluated");
    auto &output = b.add_output(socket_type, item.name, identifier).align_with_previous();
    if (socket_type_supports_fields(socket_type) && tree->type == NTREE_GEOMETRY) {
      output.field_source_reference_all();
    }
    output.structure_type(StructureType::Dynamic);
  }
  b.add_input<decl::Extend>("", "__extend__expression_input")
      .structure_type(StructureType::Dynamic)
      .hide_socket_icon(tree->type == NTREE_COMPOSIT);
  b.add_output<decl::Extend>("", "__extend__expression_output")
      .align_with_previous()
      .structure_type(StructureType::Dynamic)
      .align_with_previous();

  auto &inputs_panel = b.add_panel("Inputs");
  for (const int i : IndexRange(storage.input_items.items_num)) {
    const NodeExpressionInputItem &item = storage.input_items.items[i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const std::string identifier = ExpressionInputItemsAccessor::socket_identifier_for_item(item);
    auto &input = inputs_panel.add_input(socket_type, item.name, identifier)
                      .socket_name_ptr(
                          &tree->id, ExpressionInputItemsAccessor::item_srna, &item, "name");
    if (socket_type_supports_fields(socket_type) && tree->type == NTREE_GEOMETRY) {
      input.supports_field();
    }
    input.structure_type(StructureType::Dynamic);
  }
  inputs_panel.add_input<decl::Extend>("", "__extend__input")
      .structure_type(StructureType::Dynamic);
}

static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &ntree = *id_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *ptr->data_as<bNode>();
  if (uiLayout *panel = layout->panel(C, "expression_items", false, IFACE_("Expression Items"))) {
    socket_items::ui::draw_items_list_with_operators<ExpressionItemsAccessor>(
        C, panel, ntree, node);
    socket_items::ui::draw_active_item_props<ExpressionItemsAccessor>(
        ntree, node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
  if (uiLayout *panel = layout->panel(C, "input_items", false, IFACE_("Input Items"))) {
    socket_items::ui::draw_items_list_with_operators<ExpressionInputItemsAccessor>(
        C, panel, ntree, node);
    socket_items::ui::draw_active_item_props<ExpressionInputItemsAccessor>(
        ntree, node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  auto *storage = MEM_callocN<NodeExpression>(__func__);
  node->storage = storage;

  storage->expression_items.items = MEM_calloc_arrayN<NodeExpressionItem>(1, __func__);
  NodeExpressionItem &item = storage->expression_items.items[0];
  item.name = BLI_strdup(DATA_("Expression"));
  item.socket_type = SOCK_RGBA;
  item.identifier = storage->expression_items.next_identifier++;
  storage->expression_items.items_num = 1;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ExpressionInputItemsAccessor>(*node);
  socket_items::destruct_array<ExpressionItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeExpression &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_dupallocN<NodeExpression>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<ExpressionInputItemsAccessor>(*src_node, *dst_node);
  socket_items::copy_array<ExpressionItemsAccessor>(*src_node, *dst_node);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ExpressionInputItemsAccessor>();
  socket_items::ops::make_common_operators<ExpressionItemsAccessor>();
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<ExpressionInputItemsAccessor>(&writer, node);
  socket_items::blend_write<ExpressionItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<ExpressionInputItemsAccessor>(&reader, node);
  socket_items::blend_read_data<ExpressionItemsAccessor>(&reader, node);
}

static bool node_insert_link(bke::NodeInsertLinkParams &params)
{
  if (!socket_items::try_add_item_via_any_extend_socket<ExpressionItemsAccessor>(
          params.ntree, params.node, params.node, params.link, "__extend__expression_input"))
  {
    return false;
  }
  if (!socket_items::try_add_item_via_any_extend_socket<ExpressionItemsAccessor>(
          params.ntree, params.node, params.node, params.link, "__extend__expression_output"))
  {
    return false;
  }
  return socket_items::try_add_item_via_any_extend_socket<ExpressionInputItemsAccessor>(
      params.ntree, params.node, params.node, params.link);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  common_node_type_base(&ntype, "NodeExpression");
  ntype.ui_name = "Expression";
  ntype.ui_description = "Evaluate an expression on inputs";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype, "NodeExpression", node_free_storage, node_copy_storage);
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  ntype.register_operators = node_operators;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.insert_link = node_insert_link;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_expression_cc

namespace blender::nodes {

StructRNA *ExpressionInputItemsAccessor::item_srna = &RNA_NodeExpressionInputItem;
StructRNA *ExpressionItemsAccessor::item_srna = &RNA_NodeExpressionItem;

void ExpressionInputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ExpressionInputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

void ExpressionItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ExpressionItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
