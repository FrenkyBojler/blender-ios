/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ListBase-backed LOD helpers
 */

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "DNA_object_types.h"

#include "BKE_object.hh"

#include "DEG_depsgraph.hh"

// using Object;
// using Lod;

namespace blender {

struct Object;
struct Lod;

/* -------------------------------------------------------------------- */
/** \name Add LOD
 * \{ */

void BKE_object_lod_add(Object *ob)
{
  BLI_assert(ob != nullptr);

  Lod *lod = static_cast<Lod *>(MEM_callocN(sizeof(Lod), __func__));

  lod->distance = 25.0f;
  lod->target = nullptr;

  BLI_addtail(&ob->lod_items, lod);

  ob->act_lod = BLI_listbase_count(&ob->lod_items) - 1;

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

  BLI_freelinkN(&ob->lod_items, lod);

  const int new_count = BLI_listbase_count(&ob->lod_items);
  if (new_count == 0) {
    ob->act_lod = 0;
  }
  else if (ob->act_lod >= new_count) {
    ob->act_lod = new_count - 1;
  }

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  return true;
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Clear LODs
 * \{ */

void BKE_object_lod_clear(Object *ob)
{
  BLI_assert(ob != nullptr);

  BLI_freelistN(&ob->lod_items);
  ob->act_lod = 0;

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Index helpers
 * \{ */

Lod *BKE_object_lod_by_index(Object *ob, int index)
{
  if (!ob || index < 0) {
    return nullptr;
  }
  return static_cast<Lod *>(BLI_findlink(&ob->lod_items, index));
}

int BKE_object_lod_index_of(Object *ob, const Lod *lod_find)
{
  if (!ob || !lod_find) {
    return -1;
  }

  int i = 0;
  for (Lod &lod : ob->lod_items) {
    if (&lod == lod_find) {
      return i;
    }
    i++;
  }
  return -1;
}

/** \} */

} // namespace blender