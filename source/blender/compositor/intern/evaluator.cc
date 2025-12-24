/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "COM_context.hh"
#include "COM_evaluator.hh"
#include "COM_node_group_operation.hh"

namespace blender::compositor {

Evaluator::Evaluator(Context &context) : context_(context) {}

void Evaluator::evaluate()
{
  NodeGroupOperation node_group_operation(context_, context_.get_node_tree());
  node_group_operation.evaluate();

  if (context_.profiler()) {
    context_.profiler()->finalize(context_.get_node_tree());
  }
}

}  // namespace blender::compositor
