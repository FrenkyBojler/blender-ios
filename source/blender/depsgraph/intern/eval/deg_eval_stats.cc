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

struct TimeInterval {
  double start;
  double end;
};

static double total_merged_interval_time(MutableSpan<TimeInterval> intervals)
{
  if (intervals.is_empty()) {
    return 0.0;
  }

  /* Sort by start time. */
  std::sort(intervals.begin(), intervals.end(), [](const TimeInterval &a, const TimeInterval &b) {
    return a.start < b.start;
  });

  double total_time = 0.0;
  const TimeInterval &current_interval = intervals.first();
  double current_start = current_interval.start;
  double current_end = current_interval.end;
  for (const auto &[start, end] : intervals.drop_front(1)) {
    if (start <= current_end) {
      /* Intervals overlap, so merge the ends. */
      current_end = std::max(current_end, end);
    }
    else {
      /* Intervals don't overlap, so add to the total and move to new interval. */
      total_time += current_end - current_start;
      current_start = start;
      current_end = end;
    }
  }

  /* Add the last interval. */
  total_time += current_end - current_start;

  return total_time;
}

void deg_eval_stats_aggregate(Depsgraph *graph)
{
  Map<IDNode *, Vector<OperationNode *>> ops_by_id;
  for (OperationNode *op_node : graph->operations) {
    /* Set the time of the operation node. */
    op_node->stats.current_time = op_node->stats.end_eval_time - op_node->stats.start_eval_time;
    ComponentNode *comp_node = op_node->owner;
    IDNode *id_node = comp_node->owner;
    ops_by_id.lookup_or_add(id_node, {op_node}).append(op_node);
  }
  /* Reset current evaluation stats for ID and component nodes.
   * Those are not filled in by the evaluation engine. */
  for (Node *node : graph->id_nodes) {
    IDNode *id_node = (IDNode *)node;
    for (ComponentNode *comp_node : id_node->components.values()) {
      comp_node->stats.reset_current();
    }
    id_node->stats.reset_current();
  }

  /* Now accumulate operation timings to components. */
  for (OperationNode *op_node : graph->operations) {
    ComponentNode *comp_node = op_node->owner;
    comp_node->stats.current_time += op_node->stats.current_time;
  }

  for (const auto &[id_node, op_nodes] : ops_by_id.items()) {
    Array<TimeInterval> intervals(op_nodes.size());
    for (const int i : op_nodes.index_range()) {
      const OperationNode *op = op_nodes[i];
      intervals[i] = {op->stats.start_eval_time, op->stats.end_eval_time};
    }
    id_node->stats.current_time = total_merged_interval_time(intervals.as_mutable_span());
  }
}

}  // namespace blender::deg
