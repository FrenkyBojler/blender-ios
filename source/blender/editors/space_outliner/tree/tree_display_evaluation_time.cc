/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "BKE_scene.hh"

#include "DNA_outliner_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph_query.hh"

#include "../outliner_intern.hh"
#include "common.hh"
#include "tree_display.hh"

namespace blender::ed::outliner {

TreeDisplayEvaluationTime::TreeDisplayEvaluationTime(SpaceOutliner &space_outliner)
    : AbstractTreeDisplay(space_outliner)
{
}

ListBase TreeDisplayEvaluationTime::build_tree(const TreeSourceData &source_data)
{
  ListBase tree = {nullptr};

  /* Get the depsgraph for the source data. */
  Scene *scene = source_data.scene;
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(
      source_data.bmain, scene, source_data.view_layer);

  TreeElement *telem = add_element(
      &tree, nullptr, (void *)&scene->id, nullptr, TSE_DEPSGRAPH_ID_NODE, -1);

  DEG_foreach_dependent_ID(depsgraph, &scene->id, [&](ID *id_orig) {
    add_element(&telem->subtree, nullptr, (void *)id_orig, telem, TSE_DEPSGRAPH_ID_NODE, 0);
  });

  return tree;
}

}  // namespace blender::ed::outliner
