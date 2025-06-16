/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_node_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_enums.h"

#include "WM_api.hh"

#include "BKE_compute_context_cache.hh"
#include "BKE_context.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "ED_node.hh"
#include "ED_screen.hh"

#include "BLI_listbase.h"

#include "NOD_geo_bundle.hh"
#include "NOD_geo_closure.hh"
#include "NOD_socket_items.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

void sync_sockets_evaluate_closure(SpaceNode &snode, bNode &evaluate_closure_node)
{
  snode.edittree->ensure_topology_cache();
  bNodeSocket &closure_socket = evaluate_closure_node.input_socket(0);

  bke::ComputeContextCache compute_context_cache;
  const ComputeContext *current_context = ed::space_node::compute_context_for_edittree_socket(
      snode, compute_context_cache, closure_socket);
  if (!current_context) {
    /* The current tree does not have a known context, e.g. it is pinned but the modifier has been
     * removed. */
    return;
  }
  const Vector<const bNode *> closure_origin_nodes =
      ed::space_node::gather_linked_closure_origin_nodes(
          current_context, closure_socket, compute_context_cache);
  if (closure_origin_nodes.is_empty()) {
    return;
  }
  nodes::socket_items::clear<nodes::EvaluateClosureInputItemsAccessor>(evaluate_closure_node);
  nodes::socket_items::clear<nodes::EvaluateClosureOutputItemsAccessor>(evaluate_closure_node);

  const bNode &closure_node = *closure_origin_nodes[0];
  const NodeGeometryClosureOutput &closure_storage =
      *static_cast<const NodeGeometryClosureOutput *>(closure_node.storage);

  for (const int i : IndexRange(closure_storage.input_items.items_num)) {
    const NodeGeometryClosureInputItem &item = closure_storage.input_items.items[i];
    nodes::socket_items::add_item_with_socket_type_and_name<
        nodes::EvaluateClosureInputItemsAccessor>(
        evaluate_closure_node, eNodeSocketDatatype(item.socket_type), item.name);
  }
  for (const int i : IndexRange(closure_storage.output_items.items_num)) {
    const NodeGeometryClosureOutputItem &item = closure_storage.output_items.items[i];
    nodes::socket_items::add_item_with_socket_type_and_name<
        nodes::EvaluateClosureOutputItemsAccessor>(
        evaluate_closure_node, eNodeSocketDatatype(item.socket_type), item.name);
  }
  BKE_ntree_update_tag_node_property(snode.edittree, &evaluate_closure_node);
}

void sync_sockets_separate_bundle(SpaceNode &snode, bNode &separate_bundle_node)
{
  snode.edittree->ensure_topology_cache();
  bNodeSocket &bundle_socket = separate_bundle_node.input_socket(0);

  bke::ComputeContextCache compute_context_cache;
  const ComputeContext *current_context = ed::space_node::compute_context_for_edittree_socket(
      snode, compute_context_cache, bundle_socket);
  if (!current_context) {
    /* The current tree does not have a known context, e.g. it is pinned but the modifier has been
     * removed. */
    return;
  }
  const Vector<const bNode *> combine_bundle_nodes =
      ed::space_node::gather_linked_combine_bundle_nodes(
          current_context, bundle_socket, compute_context_cache);
  if (combine_bundle_nodes.is_empty()) {
    return;
  }
  nodes::socket_items::clear<nodes::SeparateBundleItemsAccessor>(separate_bundle_node);

  Set<StringRef> added_names;
  for (const bNode *combine_bundle_node : combine_bundle_nodes) {
    const NodeGeometryCombineBundle &combine_bundle_storage =
        *static_cast<const NodeGeometryCombineBundle *>(combine_bundle_node->storage);
    for (const int i : IndexRange(combine_bundle_storage.items_num)) {
      const NodeGeometryCombineBundleItem &item = combine_bundle_storage.items[i];
      if (!added_names.add(item.name)) {
        continue;
      }
      nodes::socket_items::add_item_with_socket_type_and_name<nodes::SeparateBundleItemsAccessor>(
          separate_bundle_node, eNodeSocketDatatype(item.socket_type), item.name);
    }
  }
  BKE_ntree_update_tag_node_property(snode.edittree, &separate_bundle_node);
}

void sync_sockets_combine_bundle(SpaceNode &snode, bNode &combine_bundle_node)
{
  snode.edittree->ensure_topology_cache();
  bNodeSocket &bundle_socket = combine_bundle_node.output_socket(0);

  bke::ComputeContextCache compute_context_cache;
  const ComputeContext *current_context = ed::space_node::compute_context_for_edittree_socket(
      snode, compute_context_cache, bundle_socket);
  if (!current_context) {
    /* The current tree does not have a known context, e.g. it is pinned but the modifier has been
     * removed. */
    return;
  }
  const Vector<const bNode *> separate_bundle_nodes =
      ed::space_node::gather_linked_separate_bundle_nodes(
          current_context, bundle_socket, compute_context_cache);
  if (separate_bundle_nodes.is_empty()) {
    return;
  }
  nodes::socket_items::clear<nodes::CombineBundleItemsAccessor>(combine_bundle_node);

  Set<StringRef> added_names;
  for (const bNode *separate_bundle_node : separate_bundle_nodes) {
    const NodeGeometrySeparateBundle &separate_bundle_storage =
        *static_cast<const NodeGeometrySeparateBundle *>(separate_bundle_node->storage);
    for (const int i : IndexRange(separate_bundle_storage.items_num)) {
      const NodeGeometrySeparateBundleItem &item = separate_bundle_storage.items[i];
      if (!added_names.add(item.name)) {
        continue;
      }
      nodes::socket_items::add_item_with_socket_type_and_name<nodes ::CombineBundleItemsAccessor>(
          combine_bundle_node, eNodeSocketDatatype(item.socket_type), item.name);
    }
  }
  BKE_ntree_update_tag_node_property(snode.edittree, &combine_bundle_node);
}

static wmOperatorStatus sockets_sync_exec(bContext *C, wmOperator * /*op*/)
{
  Main &bmain = *CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  if (!snode.edittree) {
    return OPERATOR_CANCELLED;
  }
  LISTBASE_FOREACH (bNode *, node, &snode.edittree->nodes) {
    if (!(node->flag & NODE_SELECT)) {
      continue;
    }
    if (node->is_type("GeometryNodeEvaluateClosure")) {
      sync_sockets_evaluate_closure(snode, *node);
    }
    else if (node->is_type("GeometryNodeSeparateBundle")) {
      sync_sockets_separate_bundle(snode, *node);
    }
    else if (node->is_type("GeometryNodeCombineBundle")) {
      sync_sockets_combine_bundle(snode, *node);
    }
  }
  BKE_main_ensure_invariants(bmain, snode.edittree->id);
  return OPERATOR_FINISHED;
}

void NODE_OT_sockets_sync(wmOperatorType *ot)
{
  ot->name = "Sync Sockets";
  ot->idname = "NODE_OT_sockets_sync";
  ot->description = "Update sockets to match what is actually used";

  ot->poll = ED_operator_node_editable;
  ot->exec = sockets_sync_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

}  // namespace blender::ed::space_node
