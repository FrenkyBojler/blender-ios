/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "COM_context.hh"
#include "COM_node_group_operation.hh"

namespace blender::compositor {

/* Evaluates the given compositor node group in the given context. */
void evaluate(Context &context,
              const bNodeTree &node_group,
              const NodeGroupOutputTypes needed_outputs);

}  // namespace blender::compositor
