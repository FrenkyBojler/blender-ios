/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

struct bNodeTree;

namespace blender::nodes {

struct InlineShaderNodeTreeSettings {
  bool unroll_loops;
};

bool inline_shader_node_tree(const bNodeTree &src_tree,
                             bNodeTree &dst_tree,
                             InlineShaderNodeTreeSettings settings);

}  // namespace blender::nodes
