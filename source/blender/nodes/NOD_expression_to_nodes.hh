/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_expression_ast.hh"

#include "BLI_map.hh"

struct bNodeSocket;
struct bNodeTree;

namespace blender::nodes::expression {

struct ToNodesResult {
  bNodeSocket *output_socket = nullptr;
  std::string error;
};

ToNodesResult ast_to_nodes(bNodeTree &tree, const Map<StringRef, bNodeSocket *> &inputs);

}  // namespace blender::nodes::expression
