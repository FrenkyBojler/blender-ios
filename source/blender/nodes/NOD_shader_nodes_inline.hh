/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

struct bNodeTree;

namespace blender::nodes {

struct InlineShaderNodeTreeParams {
  bool allow_preserving_repeat_zones = false;
};

bool inline_shader_node_tree(const bNodeTree &src_tree,
                             bNodeTree &dst_tree,
                             const InlineShaderNodeTreeParams &params = {});

}  // namespace blender::nodes
