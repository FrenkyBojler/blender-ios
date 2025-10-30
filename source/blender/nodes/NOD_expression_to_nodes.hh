/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_string_ref.hh"
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
                                                              StringRef tree_idname,
                                                              Span<StringRef> expressions,
                                                              Span<int> expr_indices);

}  // namespace blender::nodes::expression
