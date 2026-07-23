/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>
#include <sstream>

#include <fmt/format.h>

#include "BLI_dot_export.hh"

#include "BLO_read_write.hh"

#include "NOD_expression_parse.hh"
#include "NOD_expression_to_nodes.hh"
#include "NOD_geo_expression.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "RNA_prototypes.hh"

#include "COM_cached_expression_node_group.hh"
#include "COM_node_group_operation.hh"
#include "COM_node_operation.hh"

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
    const UString identifier{ExpressionItemsAccessor::socket_identifier_for_item(item)};
    const UString name{item.name};
    b.add_input<decl::String>(name, identifier)
        .optional_label()
        .description("Expression to be evaluated");
    b.add_output(socket_type, name, identifier)
        .align_with_previous()
        .propagate_all()
        .structure_type(StructureType::Dynamic);
  }
  b.add_input<decl::Extend>(""_ustr, "__extend__expression_input"_ustr)
      .structure_type(StructureType::Dynamic)
      .custom_draw(socket_items::ui::draw_extend_socket_fn<ExpressionItemsAccessor>());
  b.add_output<decl::Extend>(""_ustr, "__extend__expression_output"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();

  auto &inputs_panel = b.add_panel("Inputs"_ustr);
  for (const int i : IndexRange(storage.input_items.items_num)) {
    const NodeExpressionInputItem &item = storage.input_items.items[i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const UString identifier{ExpressionInputItemsAccessor::socket_identifier_for_item(item)};
    const UString name{item.name};
    inputs_panel.add_input(socket_type, name, identifier)
        .socket_name_ptr(&tree->id, *ExpressionInputItemsAccessor::item_srna, &item, "name")
        .structure_type(StructureType::Dynamic);
  }
  inputs_panel.add_input<decl::Extend>(""_ustr, "__extend__input"_ustr)
      .structure_type(StructureType::Dynamic)
      .custom_draw(socket_items::ui::draw_extend_socket_fn<ExpressionInputItemsAccessor>());
}

static void node_layout_ex(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  bNodeTree &ntree = *id_cast<bNodeTree *>(ptr->owner_id);
  bNode &node = *ptr->data_as<bNode>();
  if (ui::Layout *panel = layout.panel(C, "expression_items", false, IFACE_("Expression Items"))) {
    socket_items::ui::draw_items_list_with_operators<ExpressionItemsAccessor>(
        C, panel, ntree, node);
    socket_items::ui::draw_active_item_props<ExpressionItemsAccessor>(
        ntree, node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
        });
  }
  if (ui::Layout *panel = layout.panel(C, "input_items", false, IFACE_("Input Items"))) {
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
  auto *storage = MEM_new<NodeExpression>(__func__);
  node->storage = storage;

  storage->expression_items.items = MEM_new_array<NodeExpressionItem>(1, __func__);
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
  NodeExpression &storage = node_storage(*node);
  MEM_delete(&storage);
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeExpression &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_new<NodeExpression>(__func__, dna::shallow_copy(src_storage));
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

using namespace blender::compositor;

class ExpressionOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const expression::ExpressionNodeGroup &expression_node_group =
        this->get_expression_node_group();
    if (!expression_node_group.tree) {
      /* TODO: Error message. */
      this->allocate_default_remaining_outputs();
      return;
    }

    const bNodeTree &node_group = *expression_node_group.tree;

    const bke::GroupNodeComputeContext compute_context(
        &this->get_compute_context(), this->node().identifier, &this->node().owner_tree());
    NodeGroupOperation operation(
        this->context(), node_group, NodeGroupOutputTypes::None, compute_context);

    this->set_reference_counts(operation, node_group);
    Vector<std::unique_ptr<Result>> temporary_inputs = this->map_inputs(operation, node_group);
    operation.evaluate();
    this->write_outputs(operation, node_group);
  }

  /* Sets the reference counts of the node group operation according to the needed status of the
   * outputs of the expression node. */
  void set_reference_counts(Operation &operation, const bNodeTree &node_group)
  {
    const NodeExpression &storage = node_storage(this->node());
    node_group.ensure_interface_cache();

    for (const int i : IndexRange(storage.expression_items.items_num)) {
      const NodeExpressionItem &item = storage.expression_items.items[i];
      const std::string identifier = ExpressionItemsAccessor::socket_identifier_for_item(item);

      const bNodeTreeInterfaceSocket *interface_socket = node_group.interface_outputs()[i];

      Result &node_group_result = operation.get_result(interface_socket->identifier);
      Result &expression_node_result = this->get_result(identifier);
      node_group_result.set_reference_count(expression_node_result.should_compute() ? 1 : 0);
    }
  }

  /* Maps the input results of the node group operation to this expression node's inputs through
   * temporary results that share the data of the this group's inputs. */
  Vector<std::unique_ptr<Result>> map_inputs(Operation &operation, const bNodeTree &node_group)
  {
    const NodeExpression &storage = node_storage(this->node());
    node_group.ensure_interface_cache();

    Vector<std::unique_ptr<Result>> temporary_inputs;
    for (const int i : IndexRange(storage.input_items.items_num)) {
      const NodeExpressionInputItem &item = storage.input_items.items[i];
      const std::string identifier = ExpressionInputItemsAccessor::socket_identifier_for_item(
          item);

      const bNodeTreeInterfaceSocket *interface_socket = node_group.interface_inputs()[i];

      const Result &input_result = this->get_input(identifier);
      std::unique_ptr<Result> temporary_input = std::make_unique<Result>(
          this->context().create_result(input_result.type(), input_result.precision()));
      temporary_input->share_data(input_result);
      temporary_inputs.append(std::move(temporary_input));
      operation.map_input_to_result(interface_socket->identifier, temporary_inputs.last().get());
    }

    return temporary_inputs;
  }

  /* Writes the output results of the node group operation to this expression node operation by
   * sharing its data and freeing the results. */
  void write_outputs(Operation &operation, const bNodeTree &node_group)
  {
    const NodeExpression &storage = node_storage(this->node());
    node_group.ensure_interface_cache();

    for (const int i : IndexRange(storage.expression_items.items_num)) {
      const NodeExpressionItem &item = storage.expression_items.items[i];
      const std::string identifier = ExpressionItemsAccessor::socket_identifier_for_item(item);

      const bNodeTreeInterfaceSocket *interface_socket = node_group.interface_outputs()[i];

      Result &node_group_result = operation.get_result(interface_socket->identifier);
      Result &expression_node_result = this->get_result(identifier);
      if (expression_node_result.should_compute()) {
        expression_node_result.share_data(node_group_result);
        node_group_result.release();
      }
    }
  }

  const expression::ExpressionNodeGroup &get_expression_node_group()
  {
    const NodeExpression &storage = node_storage(this->node());

    Array<std::string> expressions(storage.expression_items.items_num);
    for (const int i : IndexRange(storage.expression_items.items_num)) {
      const NodeExpressionItem &item = storage.expression_items.items[i];
      const std::string identifier = ExpressionItemsAccessor::socket_identifier_for_item(item);
      expressions[i] = this->get_input(identifier).get_single_value_default<std::string>();
    }

    Array<StringRef> expressions_references(storage.expression_items.items_num);
    for (const int i : IndexRange(storage.expression_items.items_num)) {
      expressions_references[i] = expressions[i];
    }

    return this->context().cache_manager().expression_node_groups.get(this->node(),
                                                                      expressions_references);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new ExpressionOperation(context, node);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  common_node_type_base(&ntype, "NodeExpression"_ustr);
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
  ntype.get_compositor_operation = get_compositor_operation;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_expression_cc

namespace blender::nodes {

StructRNA **ExpressionInputItemsAccessor::item_srna = &RNA_NodeExpressionInputItem;
StructRNA **ExpressionItemsAccessor::item_srna = &RNA_NodeExpressionItem;

void ExpressionInputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  writer->write_string(item.name);
}

void ExpressionInputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

void ExpressionItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  writer->write_string(item.name);
}

void ExpressionItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
