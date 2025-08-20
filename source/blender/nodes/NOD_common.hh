/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "BLI_string_ref.hh"

#include <optional>
#include <string>

struct bNode;
struct bNodeSocket;
struct ID;
struct Main;

namespace blender::nodes {
class NodeDeclarationBuilder;
}  // namespace blender::nodes

bNodeSocket *node_group_find_input_socket(bNode *groupnode, blender::StringRef identifier);
bNodeSocket *node_group_find_output_socket(bNode *groupnode, blender::StringRef identifier);

bNodeSocket *node_group_input_find_socket(bNode *node, blender::StringRef identifier);
bNodeSocket *node_group_output_find_socket(bNode *node, blender::StringRef identifier);

namespace blender::nodes {

void node_group_declare(NodeDeclarationBuilder &b);

/**
 * Make the path absolute, by resolving it relative to the blendfile containing the owner_id.
 *
 * Empty paths are returned as std::nullopt. Absolute paths are returned as-is.
 *
 * This would be nice to have in BLI_path_utils.hh, but the implementation requires DNA_ID.hh,
 * which is not allowed to be included from BLI.
 */
std::optional<std::string> path_abs_via_id(const blender::StringRefNull path,
                                           const Main &bmain,
                                           const ID &owner_id);

}  // namespace blender::nodes
