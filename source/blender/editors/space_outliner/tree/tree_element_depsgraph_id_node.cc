/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_ID.h"
#include "DNA_outliner_types.h"

#include "../outliner_intern.hh"

#include "tree_element_depsgraph_id_node.hh"

namespace blender::ed::outliner {

TreeElementDepsgraphIDNode::TreeElementDepsgraphIDNode(TreeElement &legacy_te, const ID &orig_id)
    : AbstractTreeElement(legacy_te), orig_id_(orig_id)
{
  BLI_assert(legacy_te_.store_elem->type == TSE_DEPSGRAPH_ID_NODE);

  legacy_te_.name = orig_id.name + 2;
}

}  // namespace blender::ed::outliner
