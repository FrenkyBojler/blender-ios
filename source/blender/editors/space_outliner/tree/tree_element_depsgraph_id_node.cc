/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_ID.h"
#include "DNA_outliner_types.h"

#include "DEG_depsgraph_query.hh"

#include "../outliner_intern.hh"

#include "tree_element_depsgraph_id_node.hh"

namespace blender::ed::outliner {

TreeElementDepsgraphIDNode::TreeElementDepsgraphIDNode(TreeElement &legacy_te,
                                                       const DepsgraphIDNodeData &data)
    : AbstractTreeElement(legacy_te), depsgraph_(data.depsgraph), orig_id_(*data.orig_id)
{
  BLI_assert(legacy_te_.store_elem->type == TSE_DEPSGRAPH_ID_NODE);

  legacy_te_.name = data.orig_id->name + 2;
}

void TreeElementDepsgraphIDNode::expand(SpaceOutliner & /*soops*/) const
{
  DEG_foreach_dependent_ID(depsgraph_, &orig_id_, [&](ID *id_orig) {
    DepsgraphIDNodeData data{depsgraph_, id_orig};
    add_element(
        &legacy_te_.subtree, nullptr, (void *)&data, &legacy_te_, TSE_DEPSGRAPH_ID_NODE, 0);
  });
}

}  // namespace blender::ed::outliner
