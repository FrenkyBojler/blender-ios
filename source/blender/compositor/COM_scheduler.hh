/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_vector_set.hh"

#include "COM_context.hh"

namespace blender::compositor {

/* A type representing the ordered set of nodes defining the schedule of node execution. */
using Schedule = VectorSet<const bNode *>;

/* Computes the execution schedule of the node group. This is essentially a post-order depth first
 * traversal of the node tree from the needed output node to the leaf input nodes, with informed
 * order of traversal of dependencies based on a heuristic estimation of the number of needed
 * buffers. */
Schedule compute_schedule(const Context &context,
                          const bNodeTree &node_group,
                          OutputTypes needed_outputs);

}  // namespace blender::compositor
