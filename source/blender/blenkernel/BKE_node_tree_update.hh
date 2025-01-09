/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <functional>
#include <optional>

#include "BLI_span.hh"

struct ID;
struct ImageUser;
struct Main;
struct bNode;
struct bNodeLink;
struct bNodeSocket;
struct bNodeTree;

/**
 * Tag tree as changed without providing any more information about what has changed exactly.
 * The update process has to assume that everything may have changed.
 *
 * Using one of the methods below to tag the tree after changes is preferred when possible.
 */
void BKE_ntree_update_tag_all(bNodeTree *ntree);

/**
 * More specialized tag functions that may result in a more efficient update.
 */

void BKE_ntree_update_tag_node_property(bNodeTree *ntree, bNode *node);
void BKE_ntree_update_tag_node_new(bNodeTree *ntree, bNode *node);
void BKE_ntree_update_tag_node_removed(bNodeTree *ntree);
void BKE_ntree_update_tag_node_mute(bNodeTree *ntree, bNode *node);
void BKE_ntree_update_tag_node_internal_link(bNodeTree *ntree, bNode *node);

void BKE_ntree_update_tag_socket_property(bNodeTree *ntree, bNodeSocket *socket);
void BKE_ntree_update_tag_socket_new(bNodeTree *ntree, bNodeSocket *socket);
void BKE_ntree_update_tag_socket_type(bNodeTree *ntree, bNodeSocket *socket);
void BKE_ntree_update_tag_socket_availability(bNodeTree *ntree, bNodeSocket *socket);
void BKE_ntree_update_tag_socket_removed(bNodeTree *ntree);

void BKE_ntree_update_tag_link_changed(bNodeTree *ntree);
void BKE_ntree_update_tag_link_removed(bNodeTree *ntree);
void BKE_ntree_update_tag_link_added(bNodeTree *ntree, bNodeLink *link);
void BKE_ntree_update_tag_link_mute(bNodeTree *ntree, bNodeLink *link);

/** Used when the a new output node becomes active and therefore changes the output. */
void BKE_ntree_update_tag_active_output_changed(bNodeTree *ntree);
/** Used after file loading when run-time data on the tree has not been initialized yet. */
void BKE_ntree_update_tag_missing_runtime_data(bNodeTree *ntree);
/** Used when change parent node. */
void BKE_ntree_update_tag_parent_change(bNodeTree *ntree, bNode *node);
/** Used when an id data block changed that might be used by nodes that need to be updated. */
void BKE_ntree_update_tag_id_changed(Main *bmain, ID *id);
/** Used when an image user is updated that is used by any part of the node tree. */
void BKE_ntree_update_tag_image_user_changed(bNodeTree *ntree, ImageUser *iuser);

struct NodeTreeUpdateExtraParams {
  /**
   * Called for every tree that has been changed during the update. This can be used to send
   * notifiers to trigger redraws or depsgraph updates.
   */
  std::function<void(bNodeTree &, ID &owner)> tree_changed_fn;

  /**
   * Called for every tree whose output value may have changed based on the provided update tags.
   * This can be used to tag the depsgraph if necessary.
   */
  std::function<void(bNodeTree &, ID &owner)> tree_output_changed_fn;
};

void BKE_ntree_update(Main &bmain,
                      std::optional<blender::Span<bNodeTree *>> modified_trees = std::nullopt,
                      const NodeTreeUpdateExtraParams &params = {});
void BKE_ntree_update_after_single_tree_change(Main &bmain,
                                               bNodeTree &modified_tree,
                                               const NodeTreeUpdateExtraParams &params = {});

void BKE_ntree_update_without_main(bNodeTree &tree);
