/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "BKE_collection.hh"
#include "BKE_layer.hh"

#include "DNA_ID.h"
#include "DNA_outliner_types.h"
#include "DNA_scene_types.h"

#include "DEG_depsgraph_query.hh"

#include "../outliner_intern.hh"
#include "common.hh"

#include "tree_element_depsgraph_id_node.hh"

namespace blender::ed::outliner {

TreeElementDepsgraphIDNode::TreeElementDepsgraphIDNode(TreeElement &legacy_te,
                                                       const DepsgraphIDNodeData &data)
    : AbstractTreeElement(legacy_te), depsgraph_(data.depsgraph), orig_id_(data.orig_id)
{
  BLI_assert(legacy_te.store_elem->type == TSE_DEPSGRAPH_ID_NODE);

  if (!data.orig_id) {
    legacy_te.name = "Total";
  }
  else {
    legacy_te.name = data.orig_id->name + 2;
    legacy_te.idcode = GS(data.orig_id->name);
    if (legacy_te.idcode == ID_OB) {
      legacy_te.directdata = BKE_view_layer_base_find(DEG_get_input_view_layer(data.depsgraph),
                                                      (Object *)data.orig_id);
    }
  }
}

void TreeElementDepsgraphIDNode::expand(SpaceOutliner & /*soops*/) const
{
  if (!orig_id_) {
    DEG_foreach_ID(depsgraph_, [&](ID *orig_id) {
      DepsgraphIDNodeData data{depsgraph_, orig_id};
      add_element(&legacy_te_.subtree, orig_id, &data, &legacy_te_, TSE_DEPSGRAPH_ID_NODE, 0);
    });
    return;
  }

  switch (GS(orig_id_->name)) {
    case ID_SCE: {
      // const Scene *scene = reinterpret_cast<const Scene *>(orig_id_);
      // this->expand_scene(scene);
      break;
    }
    case ID_OB: {
      // const_cast<ID *>(orig_id_)->newid = (ID *)(&legacy_te_);
      break;
    }
    default:
      break;
  }
}

std::optional<double> TreeElementDepsgraphIDNode::node_evaluation_time() const
{
  if (!orig_id_) {
    return DEG_get_last_evaluation_time(depsgraph_);
  }
  return DEG_get_id_self_evaluation_time(depsgraph_, *orig_id_);
}

std::optional<float> TreeElementDepsgraphIDNode::node_evaluation_percent() const
{
  if (!orig_id_) {
    return 100.0f;
  }

  if (std::optional<double> id_eval_time = DEG_get_id_self_evaluation_time(depsgraph_, *orig_id_))
  {
    double eval_time = *DEG_get_last_evaluation_time(depsgraph_);
    return float(*id_eval_time / eval_time) * 100.0f;
  }
  return std::nullopt;
}

}  // namespace blender::ed::outliner
