/* SPDX-FileCopyrightText: 2011 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgpencil
 */

#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_gpencil_legacy_types.h"
#include "DNA_listBase.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.h"

#include "BKE_blender_undo.hh"
#include "BKE_gpencil_legacy.h"
#include "BKE_undo_system.hh"

#include "ED_gpencil_legacy.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "DEG_depsgraph.hh"

#include "gpencil_intern.hh"

struct bGPundonode {
  bGPundonode *next, *prev;

  char name[BKE_UNDO_STR_MAX];
  bGPdata *gpd;
};

static ListBase undo_nodes = {nullptr, nullptr};
static bGPundonode *cur_node = nullptr;

int ED_gpencil_session_active()
{
  return (BLI_listbase_is_empty(&undo_nodes) == false);
}

void gpencil_undo_init(bGPdata *gpd)
{
  gpencil_undo_push(gpd);
}

static void gpencil_undo_free_node(bGPundonode *undo_node)
{
  /* HACK: animdata wasn't duplicated, so it shouldn't be freed here,
   * or else the real copy will segfault when accessed
   */
  undo_node->gpd->adt = nullptr;

  BKE_gpencil_free_data(undo_node->gpd, false);
  MEM_freeN(undo_node->gpd);
}

void gpencil_undo_push(bGPdata *gpd)
{
  bGPundonode *undo_node;

  if (cur_node) {
    /* Remove all undone nodes from stack. */
    undo_node = cur_node->next;

    while (undo_node) {
      bGPundonode *next_node = undo_node->next;

      gpencil_undo_free_node(undo_node);
      BLI_freelinkN(&undo_nodes, undo_node);

      undo_node = next_node;
    }
  }

  /* limit number of undo steps to the maximum undo steps
   * - to prevent running out of memory during **really**
   *   long drawing sessions (triggering swapping)
   */
  /* TODO: Undo-memory constraint is not respected yet,
   * but can be added if we have any need for it. */
  if (U.undosteps && !BLI_listbase_is_empty(&undo_nodes)) {
    /* remove anything older than n-steps before cur_node */
    int steps = 0;

    undo_node = (cur_node) ? cur_node : static_cast<bGPundonode *>(undo_nodes.last);
    while (undo_node) {
      bGPundonode *prev_node = undo_node->prev;

      if (steps >= U.undosteps) {
        gpencil_undo_free_node(undo_node);
        BLI_freelinkN(&undo_nodes, undo_node);
      }

      steps++;
      undo_node = prev_node;
    }
  }

  /* create new undo node */
  undo_node = MEM_cnew<bGPundonode>("gpencil undo node");
  undo_node->gpd = BKE_gpencil_data_duplicate(nullptr, gpd, true);

  cur_node = undo_node;

  BLI_addtail(&undo_nodes, undo_node);
}

void gpencil_undo_finish()
{
  bGPundonode *undo_node = static_cast<bGPundonode *>(undo_nodes.first);

  while (undo_node) {
    gpencil_undo_free_node(undo_node);
    undo_node = undo_node->next;
  }

  BLI_freelistN(&undo_nodes);

  cur_node = nullptr;
}
