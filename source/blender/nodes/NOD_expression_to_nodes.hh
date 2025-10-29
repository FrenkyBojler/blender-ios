/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_expression_ast.hh"

#include "BLI_map.hh"
#include "BLI_utility_mixins.hh"

struct bNodeSocket;
struct bNodeTree;
struct bNode;

namespace blender::nodes::expression {

class ExpressionNodeGroup : NonCopyable, NonMovable {
 public:
  ~ExpressionNodeGroup();

  const bNodeTree *tree = nullptr;
  std::string error;
};

std::shared_ptr<ExpressionNodeGroup> expression_node_to_group(const bNode &node,
                                                              StringRef expression,
                                                              int expr_index);

}  // namespace blender::nodes::expression
