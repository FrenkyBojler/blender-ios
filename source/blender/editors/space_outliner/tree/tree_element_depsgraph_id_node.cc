/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "BKE_collection.hh"

#include "DNA_ID.h"
#include "DNA_outliner_types.h"

#include "DEG_depsgraph_query.hh"

#include "../outliner_intern.hh"
#include "common.hh"

#include "tree_element_depsgraph_id_node.hh"

namespace blender::ed::outliner {

TreeElementDepsgraphIDNode::TreeElementDepsgraphIDNode(TreeElement &legacy_te,
                                                       const DepsgraphIDNodeData &data)
    : AbstractTreeElement(legacy_te), depsgraph_(data.depsgraph), orig_id_(*data.orig_id)
{
  BLI_assert(legacy_te_.store_elem->type == TSE_DEPSGRAPH_ID_NODE);

  legacy_te_.name = data.orig_id->name + 2;
}

void TreeElementDepsgraphIDNode::expand_scene() const
{
  BLI_assert(GS(orig_id_.name) == ID_SCE);
  const Scene *scene = reinterpret_cast<const Scene *>(&orig_id_);
  FOREACH_SCENE_OBJECT_BEGIN ((void *)scene, ob) {
    ID *ob_id = reinterpret_cast<ID *>(ob);
    DepsgraphIDNodeData data{depsgraph_, ob_id};
    add_element(&legacy_te_.subtree, ob_id, &data, &legacy_te_, TSE_DEPSGRAPH_ID_NODE, 0);
  }
  FOREACH_SCENE_OBJECT_END;
  outliner_make_object_parent_hierarchy(&legacy_te_.subtree);
}

void TreeElementDepsgraphIDNode::expand(SpaceOutliner & /*soops*/) const
{
  switch (GS(orig_id_.name)) {
    case ID_SCE: {
      this->expand_scene();
      break;
    }
    default:
      break;
  }
}

std::optional<double> TreeElementDepsgraphIDNode::node_evaluation_time() const
{
  const double time = DEG_get_id_evaluation_time(depsgraph_, orig_id_);
  if (time > 0.0) {
    return time;
  }
  return {};
}

}  // namespace blender::ed::outliner
