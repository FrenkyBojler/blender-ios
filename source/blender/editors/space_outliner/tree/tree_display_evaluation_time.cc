/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "BKE_scene.hh"

#include "DNA_outliner_types.h"
#include "DNA_scene_types.h"

#include "../outliner_intern.hh"
#include "common.hh"
#include "tree_display.hh"
#include "tree_element_depsgraph_id_node.hh"

namespace blender::ed::outliner {

TreeDisplayEvaluationTime::TreeDisplayEvaluationTime(SpaceOutliner &space_outliner)
    : AbstractTreeDisplay(space_outliner)
{
}

ListBase TreeDisplayEvaluationTime::build_tree(const TreeSourceData &source_data)
{
  ListBase tree = {nullptr};

  /* Get the depsgraph for the source data. */
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(
      source_data.bmain, source_data.scene, source_data.view_layer);

  DepsgraphIDNodeData data{depsgraph, nullptr};
  add_element(&tree, nullptr, &data, nullptr, TSE_DEPSGRAPH_ID_NODE, -1);

  return tree;
}

}  // namespace blender::ed::outliner
