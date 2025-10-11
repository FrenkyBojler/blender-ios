/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <optional>
#include <string>

#include "DNA_node_types.h"

#include "BLI_dot_export.hh"

namespace blender::bke {

struct bNodeTreeDotGraph {
  dot_export::DirectedGraph digraph;
  Map<const bNode *, dot_export::NodeWithSocketsRef> dot_nodes;

  dot_export::DirectedEdge &add_directed_edge(const bNodeSocket &a, const bNodeSocket &b);
};

/**
 * Allows customizing how the generated dot graph looks like.
 */
class bNodeTreeToDotOptions {
 public:
  virtual std::string socket_name(const bNodeSocket &socket) const;
  virtual std::optional<std::string> socket_font_color(const bNodeSocket &socket) const;
  virtual void add_edge_attributes(const bNodeLink &link,
                                   dot_export::DirectedEdge &dot_edge) const;

  /** Add additional custom elements to the graph. */
  virtual void custom(bNodeTreeDotGraph &graph) const;
};

/**
 * Convert a node tree into the dot format. This can be visualized with tools like graphviz and is
 * very useful for debugging purposes.
 */
std::string node_tree_to_dot(const bNodeTree &tree,
                             const bNodeTreeToDotOptions &options = bNodeTreeToDotOptions());

}  // namespace blender::bke
