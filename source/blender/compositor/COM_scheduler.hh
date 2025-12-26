/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_vector_set.hh"

#include "COM_context.hh"
#include "COM_node_group_operation.hh"

namespace blender::compositor {

/* Computes the execution schedule of the node group. This is essentially a post-order depth first
 * traversal of the node tree from the needed output node to the leaf input nodes, with informed
 * order of traversal of dependencies based on a heuristic estimation of the number of needed
 * buffers. */
VectorSet<const bNode *> compute_schedule(const Context &context,
                                          const bNodeTree &node_group,
                                          NodeGroupOutputTypes needed_outputs);

}  // namespace blender::compositor
