/* SPDX-FileCopyrightText: 2017 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#include "DEG_depsgraph_query.hh"

#include "intern/eval/deg_eval_stats.h"

#include "intern/depsgraph.hh"

#include "intern/node/deg_node.hh"
#include "intern/node/deg_node_component.hh"
#include "intern/node/deg_node_id.hh"
#include "intern/node/deg_node_operation.hh"

namespace blender::deg {

void deg_eval_stats_aggregate(Depsgraph *graph)
{
  /* Reset current evaluation stats for ID and component nodes.
   * Those are not filled in by the evaluation engine. */
  for (Node *node : graph->id_nodes) {
    IDNode *id_node = (IDNode *)node;
    for (ComponentNode *comp_node : id_node->components.values()) {
      comp_node->stats.reset_current();
    }
    id_node->stats.reset_current();
  }
  /* Now accumulate operation timings to components and IDs. */
  for (OperationNode *op_node : graph->operations) {
    ComponentNode *comp_node = op_node->owner;
    IDNode *id_node = comp_node->owner;
    id_node->stats.current_time += op_node->stats.current_time;
    comp_node->stats.current_time += op_node->stats.current_time;
  }

  Map<Node *, double> accumulated_dependent_times;
  for (Node *node : graph->id_nodes) {
    IDNode *id_node = (IDNode *)node;

    double &node_time = accumulated_dependent_times.lookup_or_add(node,
                                                                  id_node->stats.current_time);
    DEG_foreach_dependent_ID(
        (const ::Depsgraph *)graph, id_node->id_orig, [&](const ID *dependent_id) {
          const deg::IDNode *dependent_id_node = graph->find_id_node(dependent_id);
          if (!dependent_id_node) {
            return;
          }
          node_time += dependent_id_node->stats.current_time;
        });
  }

  for (Node *node : graph->id_nodes) {
    IDNode *id_node = (IDNode *)node;
    id_node->stats.current_dependent_time = accumulated_dependent_times.lookup(node);
  }
}

}  // namespace blender::deg
