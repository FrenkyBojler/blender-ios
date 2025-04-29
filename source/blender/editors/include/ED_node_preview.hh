/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_node_runtime.hh"
#include "BLI_map.hh"

#include "RE_pipeline.h"

#include "IMB_imbuf.hh"

#include "DNA_material_types.h"

struct ImBuf;
struct SpaceNode;
struct bContext;
struct bNode;
struct bNodeTree;
struct wmWindowManager;
struct Render;

namespace blender::ed::space_node {
using bke::UpdateCounter;
struct ShaderNodesPreviewJob;

/**
 * Used to know which state is currently being rendered.
 */
struct LastUpdate {
  /**
   * Update counter of the current nodetree. If the counter differs, it means that all node
   * previews are outdated.
   */
  UpdateCounter whole_tree_update_counter = UpdateCounter();
  /**
   * Update counter of the nodetree viewed, it is used to know if at least one node preview needs
   * to be re-rendered.
   */
  UpdateCounter any_node_updatecounter = UpdateCounter();
  /**
   * Update counter of the bNodeTreePath vector. See `get_treepath_update_counter` for more
   * details. If this counter differs, it means that at least one node have outdated previews.
   */
  UpdateCounter treepath_updatecounter = UpdateCounter();
  ePreviewType preview_type = MA_FLAT;
  int preview_size = 0;
};

/**
 * All properties of the previews present in this structure should always be corresponding to all
 * the previews cached in the `previews_map`. The size/updatecounter properties should be modified
 * only when all previews are corresponding to those properties. It may result in some more
 * refreshes, but it is the only way to make sure that the system always detect when a preview is
 * outdated.
 */
struct NestedTreePreviews {
  Render *previews_render = nullptr;
  /** Use this map to keep track of the latest #ImBuf used (after freeing the renderresult). */
  blender::Map<const int32_t, std::pair<ImBuf *, UpdateCounter>> previews_map;
  ShaderNodesPreviewJob *running_job = nullptr;

  LastUpdate last_update;

  NestedTreePreviews(const int preview_size)
  {
    this->last_update.preview_size = preview_size;
  }
  ~NestedTreePreviews()
  {
    if (this->previews_render) {
      RE_FreeRender(this->previews_render);
    }
    for (std::pair<ImBuf *, UpdateCounter> &cache : this->previews_map.values()) {
      IMB_freeImBuf(cache.first);
    }
  }
};

void free_previews(wmWindowManager &wm, SpaceNode &snode);
/**
 * \note #node_release_preview_ibuf should be called after this.
 */
ImBuf *node_preview_acquire_ibuf(bNodeTree &ntree,
                                 NestedTreePreviews &tree_previews,
                                 const bNode &node);
void node_release_preview_ibuf(NestedTreePreviews &tree_previews);
/**
 * This function returns the `NestedTreePreviews *` for the node-tree shown in the #SpaceNode.
 * This is the first function in charge of the previews by calling `ensure_nodetree_previews`.
 */
NestedTreePreviews *get_nested_previews(const bContext &C, SpaceNode &snode);

}  // namespace blender::ed::space_node
