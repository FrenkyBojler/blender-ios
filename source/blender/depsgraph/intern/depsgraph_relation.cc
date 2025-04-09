/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#include "intern/depsgraph_relation.hh" /* own include */

#include "intern/node/deg_node.hh"
#include <algorithm>

namespace blender::deg {

void Relation::unlink_and_destruct()
{
  /* Sanity check. */
  BLI_assert(from != nullptr && to != nullptr);
  from->outlinks.remove_first_occurrence_and_reorder(this);
  const auto *it = std::find_if(
      to->inlinks.begin(), to->inlinks.end(), [&](const destruct_ptr<Relation> &ptr) {
        return ptr.get() == this;
      });
  BLI_assert(it != to->inlinks.end());
  to->inlinks.remove_and_reorder(it - to->inlinks.begin());
}

}  // namespace blender::deg
