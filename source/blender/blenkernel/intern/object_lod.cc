/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ListBase-backed LOD helpers
 */

#include "MEM_guardedalloc.h"
#include "BLI_utildefines.h"
#include "BLI_listbase.h"

#include "BKE_object.hh"
#include "BKE_lib_id.hh" /* id_us_min / id_us_plus */
#include "DEG_depsgraph.hh"
#include "DNA_object_types.h"

#include <cassert>

/* Create a new Lod node, append to object's list, set default values.
 * Note: operators/UI should add WM notifiers; BKE tags depsgraph. */
// !!!(Tri): 
// void BKE_object_lod_add(Object *ob)
// {
//   if (ob == nullptr) {
//     return;
//   }

//   Lod *lod = (Lod *)MEM_callocN(sizeof(Lod), "Lod item");
//   lod->target = nullptr;
//   lod->distance = 10.0f;

//   BLI_addtail(&ob->lod_items, lod);

//   /* ensure active index points to the new item */
//   ob->act_lod = BKE_object_lod_index_of(ob, lod);

//   /* notify depsgraph of change */
//   DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
// }

void BKE_object_lod_add(Object *ob)
{
  Lod *lod = MEM_cnew<Lod>(__func__);

  lod->target = nullptr;
  lod->distance = 10.0f;

  BLI_addtail(&ob->lod_items, lod);

  ob->act_lod = BLI_listbase_count(&ob->lod_items) - 1;
}

// MARK: -  remove

/* Remove Lod at index (0-based). Returns true if removed. */
// bool BKE_object_lod_remove(Object *ob, int index)
// {
//   if (ob == nullptr) {
//     return false;
//   }

//   if (index < 0) {
//     return false;
//   }

//   /* find the node */
//   Lod *lod = BKE_object_lod_by_index(ob, index);
//   if (lod == nullptr) {
//     return false;
//   }

//   /* If the node references a target, decrement its user-count.
//    * The RNA pointer setter uses PROP_ID_REFCOUNT, so we do the symmetric step here. */
//   if (lod->target) {
//     id_us_min(reinterpret_cast<ID *>(lod->target));
//     lod->target = nullptr;
//   }

//   BLI_freelinkN(&ob->lod_items, lod);

//   /* Update active index: clamp */
//   if (BLI_listbase_is_empty(&ob->lod_items)) {
//     ob->act_lod = -1;
//   }
//   else {
//     /* clamp to last */
//     int last_index = BKE_object_lod_index_of(ob, (Lod *)ob->lod_items.last);
//     if (ob->act_lod > last_index) {
//       ob->act_lod = last_index;
//     }
//     if (ob->act_lod < 0) {
//       ob->act_lod = 0;
//     }
//   }

//   DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
//   return true;
// }

bool BKE_object_lod_remove(Object *ob, int index)
{
  Lod *lod = (Lod *)BLI_findlink(&ob->lod_items, index);
  if (!lod) {
    return false;
  }

  BLI_freelinkN(&ob->lod_items, lod);

  const int new_count = BLI_listbase_count(&ob->lod_items);

  if (new_count == 0) {
    ob->act_lod = -1;
  }
  else if (ob->act_lod >= new_count) {
    ob->act_lod = new_count - 1;
  }

  return true;
}

// MARK: - Clear

void BKE_object_lod_clear(Object *ob)
{
  if (ob == nullptr) {
    return;
  }

  for (Lod *lod = (Lod *)ob->lod_items.first; lod; lod = (Lod *)lod->next) {
    if (lod->target) {
      id_us_min(reinterpret_cast<ID *>(lod->target));
      lod->target = nullptr;
    }
  }

  BLI_freelistN(&ob->lod_items);
  ob->act_lod = -1;

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
}

/* Utility helpers */

Lod *BKE_object_lod_by_index(Object *ob, int index)
{
  if (ob == nullptr || index < 0) {
    return nullptr;
  }
  int i = 0;
  for (Lod *lod = (Lod *)ob->lod_items.first; lod; lod = (Lod *)lod->next, ++i) {
    if (i == index) {
      return lod;
    }
  }
  return nullptr;
}

int BKE_object_lod_index_of(Object *ob, Lod *lod_target)
{
  if (ob == nullptr || lod_target == nullptr) {
    return -1;
  }
  int i = 0;
  for (Lod *lod = (Lod *)ob->lod_items.first; lod; lod = (Lod *)lod->next, ++i) {
    if (lod == lod_target) {
      return i;
    }
  }
  return -1;
}
