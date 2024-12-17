/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_main_invariants.hh"
#include "BKE_node_tree_update.hh"

#include "DEG_depsgraph.hh"

#include "DNA_node_types.h"

void BKE_main_ensure_invariants(Main &bmain)
{
  NodeTreeUpdateExtraParams params;
  params.tree_changed_fn = [](bNodeTree &ntree, ID & /*owner_id*/) {
    DEG_id_tag_update(&ntree.id, ID_RECALC_SYNC_TO_EVAL);
  };
  params.tree_output_changed_fn = [](bNodeTree &ntree, ID & /*owner_id*/) {
    DEG_id_tag_update(&ntree.id, ID_RECALC_NTREE_OUTPUT);
  };

  BKE_ntree_update_main(&bmain, &params);
}
