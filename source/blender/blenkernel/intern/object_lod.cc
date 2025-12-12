/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ListBase-backed LOD helpers
 */

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "DNA_object_types.h"

#include "BKE_lib_id.hh"
#include "BKE_object.hh"

#include "DEG_depsgraph.hh"

/* -------------------------------------------------------------------- */
/** \name Add LOD
 * \{ */

// TODO(Tri): Fix 
void BKE_object_lod_add(Object *ob)
{
  // printf("[LOD_DEBUG] Before add: first=%p last=%p\n",
  //      ob->lod_items.first,
  //      ob->lod_items.last);

  BLI_assert(ob != nullptr);

  /* Allocate & zero the new LOD node. */
  Lod *lod = (Lod *)MEM_callocN(sizeof(Lod), "lod");

  // printf("[LOD_DEBUG] Allocated new lod=%p\n", lod);


  lod->distance = 10.0f; // 10m default
  // !!!(Tri): Wrong?
  lod->target = nullptr;

  /* Append to ListBase. */
  BLI_addtail(&ob->lod_items, lod);

  // printf("[LOD_DEBUG] After add: first=%p last=%p\n",
  //      ob->lod_items.first,
  //      ob->lod_items.last);

  // printf("[LOD_DEBUG] lod->next=%p lod->prev=%p\n",
  //      lod->next, lod->prev);

  fprintf(stderr, "[BKE_LOD] add: ob=%p new_lod=%p first=%p last=%p\n", 
    (void *)ob, (void *)lod, (void *)ob->lod_items.first, (void *)ob->lod_items.last);

  /* Set new active index. */
  ob->act_lod = BLI_listbase_count(&ob->lod_items) - 1;

  /* Mark depsgraph updated. */
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Remove LOD
 * \{ */

bool BKE_object_lod_remove(Object *ob, int index)
{
  BLI_assert(ob != nullptr);

  Lod *lod = static_cast<Lod *>(BLI_findlink(&ob->lod_items, index));
  if (lod == nullptr) {
    return false;
  }

  /* Handle ID user-count. */
  if (lod->target) {
    id_us_min(reinterpret_cast<ID *>(lod->target));
    lod->target = nullptr;
  }

  fprintf(stderr, "[BKE_LOD] remove: ob=%p remove_lod=%p\n", (void *)ob, (void *)lod);

  /* Remove from list & free. */
  BLI_freelinkN(&ob->lod_items, lod);

  const int new_count = BLI_listbase_count(&ob->lod_items);

  if (new_count == 0) {
    ob->act_lod = -1;
  }
  else if (ob->act_lod >= new_count) {
    ob->act_lod = new_count - 1;
  }

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  return true;
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Clear all LODs
 * \{ */

void BKE_object_lod_clear(Object *ob)
{
  BLI_assert(ob != nullptr);

  LISTBASE_FOREACH_MUTABLE (Lod *, lod, &ob->lod_items) {
    if (lod->target) {
      id_us_min(reinterpret_cast<ID *>(lod->target));
      lod->target = nullptr;
    }
  }

  // !!!(Tri):
  BLI_freelistN(&ob->lod_items);
  ob->act_lod = -1;

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Index helpers (optional)
 * \{ */

Lod *BKE_object_lod_by_index(Object *ob, int index)
{
  if (!ob || index < 0) {
    return nullptr;
  }
  return static_cast<Lod *>(BLI_findlink(&ob->lod_items, index));
}

int BKE_object_lod_index_of(Object *ob, Lod *lod_find)
{
  if (!ob || !lod_find) {
    return -1;
  }

  int i = 0;
  LISTBASE_FOREACH (Lod *, lod, &ob->lod_items) {
    if (lod == lod_find) {
      return i;
    }
    i++;
  }

  return -1;
}

/** \} */
