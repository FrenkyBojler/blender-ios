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

  for (const int i : IndexRange(storage.output_items.items_num)) {
    const NodeExpressionOutputItem &item = storage.output_items.items[i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const std::string identifier = ExpressionOutputItemsAccessor::socket_identifier_for_item(item);
    b.add_input<decl::String>(item.name, identifier)
        .optional_label()
        .description("Expression to be evaluated");
    auto &output = b.add_output(socket_type, item.name, identifier).align_with_previous();
    if (socket_type_supports_fields(socket_type) && tree->type == NTREE_GEOMETRY) {
      output.dependent_field().reference_pass_all();
    }
    output.structure_type(StructureType::Dynamic);
  }
  b.add_input<decl::Extend>("", "__extend__expression_input")
      .structure_type(StructureType::Dynamic);
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

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  auto *storage = MEM_callocN<NodeExpression>(__func__);
  node->storage = storage;
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ExpressionInputItemsAccessor>(*node);
  socket_items::destruct_array<ExpressionOutputItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeExpression &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_dupallocN<NodeExpression>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<ExpressionInputItemsAccessor>(*src_node, *dst_node);
  socket_items::copy_array<ExpressionOutputItemsAccessor>(*src_node, *dst_node);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ExpressionInputItemsAccessor>();
  socket_items::ops::make_common_operators<ExpressionOutputItemsAccessor>();
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  socket_items::blend_write<ExpressionInputItemsAccessor>(&writer, node);
  socket_items::blend_write<ExpressionOutputItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  socket_items::blend_read_data<ExpressionInputItemsAccessor>(&reader, node);
  socket_items::blend_read_data<ExpressionOutputItemsAccessor>(&reader, node);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const std::string expression = params.extract_input<std::string>("Expression");

  std::stringstream errors;
  ResourceScope scope;
  const expression::ast::Expr *value = expression::parse(scope, expression, errors);
  if (!value) {
    params.error_message_add(NodeWarningType::Error, errors.str());
    params.set_default_remaining_outputs();
    return;
  }

  dot_export::DirectedGraph graph;
  graph.attributes.set("ordering", "out");
  value->to_dot(graph);
  std::cout << "\n\n" << graph.to_dot_string() << "\n\n";
  params.set_default_remaining_outputs();
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  sh_geo_node_type_base(&ntype, "NodeExpression");
  ntype.ui_name = "Expression";
  ntype.ui_description = "Evaluate an expression on inputs";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.freefunc = node_free_storage;
  ntype.copyfunc = node_copy_storage;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  ntype.register_operators = node_operators;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_expression_cc

namespace blender::nodes {

StructRNA *ExpressionInputItemsAccessor::item_srna = &RNA_NodeExpressionInputItem;
StructRNA *ExpressionOutputItemsAccessor::item_srna = &RNA_NodeExpressionOutputItem;

void ExpressionInputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ExpressionInputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

void ExpressionOutputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ExpressionOutputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
