/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_expression_ast.hh"

#include "BLI_map.hh"

struct bNodeSocket;
struct bNodeTree;
struct bNode;

namespace blender::nodes::expression {

void expression_node_to_group(const bNode &node,
                              StringRef expression,
                              int expr_index,
                              bNodeTree &r_tree,
                              std::string &r_error);

}  // namespace blender::nodes::expression
