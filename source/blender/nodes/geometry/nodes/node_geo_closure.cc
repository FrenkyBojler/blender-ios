/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_string_utf8.h"

#include "NOD_geo_closure.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"

#include "BKE_compute_context_cache.hh"
#include "BKE_main_invariants.hh"

#include "BLO_read_write.hh"

namespace blender::nodes::node_geo_closure_cc {

/** Shared between closure input and output node. */
static void node_layout_ex(uiLayout *layout, bContext *C, PointerRNA *current_node_ptr)
{
  bNodeTree &ntree = *reinterpret_cast<bNodeTree *>(current_node_ptr->owner_id);
  bNode *current_node = static_cast<bNode *>(current_node_ptr->data);

  const bke::bNodeTreeZones *zones = ntree.zones();
  if (!zones) {
    return;
  }
  const bke::bNodeTreeZone *zone = zones->get_zone_by_node(current_node->identifier);
  if (!zone) {
    return;
  }
  if (!zone->output_node) {
    return;
  }
  bNode &output_node = const_cast<bNode &>(*zone->output_node);

  if (current_node->type_legacy == GEO_NODE_CLOSURE_INPUT) {
    if (uiLayout *panel = uiLayoutPanel(C, layout, "input_items", false, TIP_("Input Items"))) {
      socket_items::ui::draw_items_list_with_operators<ClosureInputItemsAccessor>(
          C, panel, ntree, output_node);
      socket_items::ui::draw_active_item_props<ClosureInputItemsAccessor>(
          ntree, output_node, [&](PointerRNA *item_ptr) {
            uiItemR(panel, item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          });
    }
  }
  else {
    if (uiLayout *panel = uiLayoutPanel(C, layout, "output_items", false, TIP_("Output Items"))) {
      socket_items::ui::draw_items_list_with_operators<ClosureOutputItemsAccessor>(
          C, panel, ntree, output_node);
      socket_items::ui::draw_active_item_props<ClosureOutputItemsAccessor>(
          ntree, output_node, [&](PointerRNA *item_ptr) {
            uiItemR(panel, item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          });
    }
  }
}

namespace input_node {

NODE_STORAGE_FUNCS(NodeGeometryClosureInput);

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  const bNodeTree *tree = b.tree_or_null();
  if (node && tree) {
    const NodeGeometryClosureInput &storage = node_storage(*node);
    const bNode *output_node = tree->node_by_id(storage.output_node_id);
    if (output_node) {
      const auto &output_storage = *static_cast<const NodeGeometryClosureOutput *>(
          output_node->storage);
      for (const int i : IndexRange(output_storage.input_items.items_num)) {
        const NodeGeometryClosureInputItem &item = output_storage.input_items.items[i];
        const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
        const std::string identifier = ClosureInputItemsAccessor::socket_identifier_for_item(item);
        b.add_output(socket_type, item.name, identifier);
      }
    }
  }
  b.add_output<decl::Extend>("", "__extend__");
}

static void node_label(const bNodeTree * /*ntree*/,
                       const bNode * /*node*/,
                       char *label,
                       const int label_maxncpy)
{
  BLI_strncpy_utf8(label, IFACE_("Closure"), label_maxncpy);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryClosureInput *data = MEM_callocN<NodeGeometryClosureInput>(__func__);
  node->storage = data;
}

static bool node_insert_link(bNodeTree *ntree, bNode *node, bNodeLink *link)
{
  bNode *output_node = ntree->node_by_id(node_storage(*node).output_node_id);
  if (!output_node) {
    return true;
  }
  return socket_items::try_add_item_via_any_extend_socket<ClosureInputItemsAccessor>(
      *ntree, *node, *output_node, *link);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeClosureInput", GEO_NODE_CLOSURE_INPUT);
  ntype.ui_name = "Closure Input";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = nullptr;
  ntype.initfunc = node_init;
  ntype.labelfunc = node_label;
  ntype.no_muting = true;
  ntype.insert_link = node_insert_link;
  ntype.draw_buttons_ex = node_layout_ex;
  blender::bke::node_type_storage(
      ntype, "NodeGeometryClosureInput", node_free_standard_storage, node_copy_standard_storage);
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace input_node

namespace output_node {

NODE_STORAGE_FUNCS(NodeGeometryClosureOutput);

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_layout([](uiLayout *layout, bContext * /*C*/, PointerRNA * /*node_ptr*/) {
    uiItemO(layout, "Sync Sockets", ICON_NONE, "NODE_OT_closure_interface_sync");
  });

  b.add_output<decl::Closure>("Closure");

  const bNodeTree *tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (node && tree) {

    const NodeGeometryClosureOutput &storage = node_storage(*node);
    for (const int i : IndexRange(storage.output_items.items_num)) {
      const NodeGeometryClosureOutputItem &item = storage.output_items.items[i];
      const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
      const std::string identifier = ClosureOutputItemsAccessor::socket_identifier_for_item(item);
      b.add_input(socket_type, item.name, identifier);
    }
  }
  b.add_input<decl::Extend>("", "__extend__");
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryClosureOutput *data = MEM_callocN<NodeGeometryClosureOutput>(__func__);
  node->storage = data;
}

static void node_copy_storage(bNodeTree * /*dst_tree*/, bNode *dst_node, const bNode *src_node)
{
  const NodeGeometryClosureOutput &src_storage = node_storage(*src_node);
  auto *dst_storage = MEM_dupallocN<NodeGeometryClosureOutput>(__func__, src_storage);
  dst_node->storage = dst_storage;

  socket_items::copy_array<ClosureInputItemsAccessor>(*src_node, *dst_node);
  socket_items::copy_array<ClosureOutputItemsAccessor>(*src_node, *dst_node);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<ClosureInputItemsAccessor>(*node);
  socket_items::destruct_array<ClosureOutputItemsAccessor>(*node);
  MEM_freeN(node->storage);
}

static bool node_insert_link(bNodeTree *ntree, bNode *node, bNodeLink *link)
{
  return socket_items::try_add_item_via_any_extend_socket<ClosureOutputItemsAccessor>(
      *ntree, *node, *node, *link);
}

static wmOperatorStatus sync_sockets_exec(bContext *C, wmOperator * /*op*/)
{
  Main &bmain = *CTX_data_main(C);
  SpaceNode *snode = CTX_wm_space_node(C);
  bNodeTree *closure_tree = snode->edittree;
  closure_tree->ensure_topology_cache();
  bNode *closure_output_node = bke::node_get_active(*closure_tree);
  const bNodeSocket &closure_socket = closure_output_node->output_socket(0);
  bke::ComputeContextCache compute_context_cache;
  const ComputeContext *source_context = ed::space_node::compute_context_for_edittree_socket(
      *snode, compute_context_cache, closure_socket);
  if (!source_context) {
    return OPERATOR_CANCELLED;
  }
  const ComputeContext *evaluation_context_generic =
      ed::space_node::compute_context_for_closure_evaluation(
          source_context, closure_socket, compute_context_cache, {});
  if (!evaluation_context_generic) {
    return OPERATOR_CANCELLED;
  }
  const auto *evaluation_context = dynamic_cast<const bke::EvaluateClosureComputeContext *>(
      evaluation_context_generic);
  const bNode *evaluation_node = evaluation_context->evaluate_node();
  if (!evaluation_node) {
    return OPERATOR_CANCELLED;
  }
  const auto *evaluate_storage = static_cast<const NodeGeometryEvaluateClosure *>(
      evaluation_node->storage);

  // TODO: reuse old ids
  socket_items::clear<ClosureInputItemsAccessor>(*closure_output_node);
  socket_items::clear<ClosureOutputItemsAccessor>(*closure_output_node);

  for (const int i : IndexRange(evaluate_storage->input_items.items_num)) {
    const NodeGeometryEvaluateClosureInputItem &evaluate_item =
        evaluate_storage->input_items.items[i];
    socket_items::add_item_with_socket_type_and_name<ClosureInputItemsAccessor>(
        *closure_output_node, eNodeSocketDatatype(evaluate_item.socket_type), evaluate_item.name);
  }
  for (const int i : IndexRange(evaluate_storage->output_items.items_num)) {
    const NodeGeometryEvaluateClosureOutputItem &evaluate_item =
        evaluate_storage->output_items.items[i];
    socket_items::add_item_with_socket_type_and_name<ClosureOutputItemsAccessor>(
        *closure_output_node, eNodeSocketDatatype(evaluate_item.socket_type), evaluate_item.name);
  }

  BKE_ntree_update_tag_node_property(closure_tree, closure_output_node);
  BKE_main_ensure_invariants(bmain, closure_tree->id);
  return OPERATOR_FINISHED;
}

static void node_operators()
{
  socket_items::ops::make_common_operators<ClosureInputItemsAccessor>();
  socket_items::ops::make_common_operators<ClosureOutputItemsAccessor>();

  WM_operatortype_append([](wmOperatorType *ot) {
    ot->name = "Sync Closure Interface";
    ot->idname = "NODE_OT_closure_interface_sync";
    ot->description = "Updates the closure zone to match the place where it is evaluated";
    ot->poll = [](bContext *C) {
      SpaceNode *snode = CTX_wm_space_node(C);
      if (!snode) {
        return false;
      }
      if (!snode->edittree) {
        return false;
      }
      bNode *active_node = bke::node_get_active(*snode->edittree);
      if (!active_node) {
        return false;
      }
      if (!active_node->is_type("GeometryNodeClosureOutput")) {
        return false;
      }
      return true;
    };
    ot->exec = sync_sockets_exec;
  });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeClosureOutput", GEO_NODE_CLOSURE_OUTPUT);
  ntype.ui_name = "Closure Output";
  ntype.nclass = NODE_CLASS_INTERFACE;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.labelfunc = input_node::node_label;
  ntype.no_muting = true;
  ntype.register_operators = node_operators;
  ntype.insert_link = node_insert_link;
  ntype.draw_buttons_ex = node_layout_ex;
  bke::node_type_storage(ntype, "NodeGeometryClosureOutput", node_free_storage, node_copy_storage);
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace output_node

}  // namespace blender::nodes::node_geo_closure_cc

namespace blender::nodes {

StructRNA *ClosureInputItemsAccessor::item_srna = &RNA_NodeGeometryClosureInputItem;
int ClosureInputItemsAccessor::node_type = GEO_NODE_CLOSURE_OUTPUT;
int ClosureInputItemsAccessor::item_dna_type = SDNA_TYPE_FROM_STRUCT(NodeGeometryClosureInputItem);

void ClosureInputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ClosureInputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

StructRNA *ClosureOutputItemsAccessor::item_srna = &RNA_NodeGeometryClosureOutputItem;
int ClosureOutputItemsAccessor::node_type = GEO_NODE_CLOSURE_OUTPUT;
int ClosureOutputItemsAccessor::item_dna_type = SDNA_TYPE_FROM_STRUCT(
    NodeGeometryClosureOutputItem);

void ClosureOutputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
}

void ClosureOutputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
}

}  // namespace blender::nodes
