/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "MEM_guardedalloc.h"
#include "BLI_utildefines.h"
#include "BLI_string.h"

#include "BKE_object.hh"
#include "BKE_lib_id.hh" /* id_us_plus / id_us_min */
#include "DEG_depsgraph.hh"
#include "DNA_object_types.h"

#include <algorithm> /* std::min, std::max */
#include <cstring>   /* memcpy */

/* Debug helper to detect common corruption cases (temporary). */
static void debug_check_lod_items(Object *ob)
{
  if (ob == nullptr) return;
#ifdef WITH_ASSERT_ABORT
  if (ob->lod_items_num > 0 && ob->lod_items == nullptr) {
    fprintf(stderr, "LOD: lod_items_num > 0 but lod_items == NULL\n");
    BLI_assert(ob->lod_items != nullptr);
  }
#endif
}

/* Grow array helper */
static LodItem *lod_items_grow(LodItem *items, int old_num, int new_num)
{
  if (items == nullptr) {
    /* allocate zeroed memory for deterministic fields */
    return static_cast<LodItem *>(MEM_callocN(sizeof(LodItem) * new_num, "object->lod_items"));
  }

  /* MEM_reallocN keeps previous contents, but newly allocated area is not guaranteed zeroed.
   * We'll use it for grow and explicitly zero the new slot(s). */
  LodItem *new_items = static_cast<LodItem *>(MEM_reallocN(items, sizeof(LodItem) * new_num));
  if (new_items == nullptr) {
    /* allocation failed; keep old pointer (shouldn't happen often). */
    return items;
  }

  /* zero newly allocated region */
  if (new_num > old_num) {
    size_t added_bytes = size_t(new_num - old_num) * sizeof(LodItem);
    memset(&new_items[old_num], 0, added_bytes);
  }
  return new_items;
}

/* Shrink array safely: allocate new block, copy, free old block.
 * This avoids potential allocator issues with realloc shrinking on some allocators
 * and makes the code clearer / safer. */
static LodItem *lod_items_shrink(LodItem *items, int old_num, int new_num)
{
  if (new_num == 0) {
    if (items) {
      MEM_freeN(items);
    }
    return nullptr;
  }

  LodItem *new_items = static_cast<LodItem *>(MEM_callocN(sizeof(LodItem) * new_num, "object->lod_items_shrink"));
  if (new_items == nullptr) {
    /* allocation failed: keep old pointer */
    return items;
  }

  /* Copy the first new_num elements */
  memcpy(new_items, items, sizeof(LodItem) * new_num);

  /* free old */
  MEM_freeN(items);
  return new_items;
}

void BKE_object_lod_add(Object *ob)
{
  if (ob == nullptr) {
    return;
  }

  const int old_num = ob->lod_items_num;
  const int new_num = old_num + 1;

  ob->lod_items = lod_items_grow(ob->lod_items, old_num, new_num);

  /* Initialize the new element */
  LodItem *it = &ob->lod_items[old_num];
  it->target = nullptr;
  it->distance = 10.0f;
  it->_pad = 0;

  ob->lod_items_num = new_num;

  /* If index was -1 (empty), clamp to 0; otherwise set to new slot */
  if (ob->lod_items_index < 0) {
    ob->lod_items_index = 0;
  }
  else {
    ob->lod_items_index = old_num;
  }

  /* mark depsgraph so viewers update */
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
}

bool BKE_object_lod_remove(Object *ob, int index)
{
  if (ob == nullptr) {
    return false;
  }
  if (ob->lod_items == nullptr || ob->lod_items_num <= 0) {
    return false;
  }
  if ((index < 0) || (index >= ob->lod_items_num)) {
    return false;
  }

  /* If the item had a target, decrement its user count.
   * The RNA pointer setter should have incremented the ID users when target was set,
   * so here we are symmetric and decrement. */
  if (ob->lod_items[index].target) {
    id_us_min(reinterpret_cast<ID *>(ob->lod_items[index].target));
    ob->lod_items[index].target = nullptr;
  }

  const int old_num = ob->lod_items_num;
  const int new_num = old_num - 1;

  /* Move items down in memory before shrinking. We'll copy to new block to avoid
   * allocator shrink oddities. */
  if (index < new_num) {
    /* shift inline so new_items_shrink copy doesn't need to skip the removed element */
    for (int i = index; i < old_num - 1; ++i) {
      ob->lod_items[i] = ob->lod_items[i + 1];
    }
  }

  ob->lod_items_num = new_num;

  if (new_num == 0) {
    /* free block */
    MEM_freeN(ob->lod_items);
    ob->lod_items = nullptr;
    ob->lod_items_index = -1;
  }
  else {
    ob->lod_items = lod_items_shrink(ob->lod_items, old_num, new_num);
    /* clamp index */
    if (ob->lod_items_index >= ob->lod_items_num) {
      ob->lod_items_index = ob->lod_items_num - 1;
    }
    if (ob->lod_items_index < 0) {
      ob->lod_items_index = 0;
    }
  }

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  return true;
}

void BKE_object_lod_clear(Object *ob)
{
  if (ob == nullptr) {
    return;
  }
  if (ob->lod_items == nullptr) {
    ob->lod_items_num = 0;
    ob->lod_items_index = -1;
    return;
  }

  /* Decrement ID users for targets */
  for (int i = 0; i < ob->lod_items_num; ++i) {
    if (ob->lod_items[i].target) {
      id_us_min(reinterpret_cast<ID *>(ob->lod_items[i].target));
      ob->lod_items[i].target = nullptr;
    }
  }

  MEM_freeN(ob->lod_items);
  ob->lod_items = nullptr;
  ob->lod_items_num = 0;
  ob->lod_items_index = -1;

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
}
