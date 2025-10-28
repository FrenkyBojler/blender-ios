/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_expression_to_nodes.hh"

namespace blender::nodes::expression {

ToNodesResult ast_to_nodes(bNodeTree &tree, const Map<StringRef, bNodeSocket *> &inputs)
{
  ToNodesResult result;
  /* TODO */
  UNUSED_VARS(tree, inputs);
  result.error = "Not implemented";
  return result;
}

}  // namespace blender::nodes::expression
