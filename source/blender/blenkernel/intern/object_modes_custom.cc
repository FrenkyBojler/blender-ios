/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Registry of addon-registered object modes, see #BKE_object_modes.hh.
 */

#include "MEM_guardedalloc.h"

#include "BLI_listbase.hh"
#include "BLI_utildefines.hh"

#include "DNA_object_types.h"

#include "BKE_object_modes.hh"

namespace blender {

static ListBaseT<ObjectModeType> g_object_mode_types = {nullptr, nullptr};

bool BKE_object_mode_type_add(ObjectModeType *mt)
{
  if (mt->idname[0] == '\0') {
    return false;
  }
  if (BKE_object_mode_type_find(mt->idname) != nullptr) {
    return false;
  }
  BLI_addtail(&g_object_mode_types, mt);
  return true;
}

void BKE_object_mode_type_remove(ObjectModeType *mt)
{
  BLI_remlink(&g_object_mode_types, mt);
  MEM_delete(mt);
}

ObjectModeType *BKE_object_mode_type_find(const char *idname)
{
  for (ObjectModeType &mt : g_object_mode_types) {
    if (STREQ(mt.idname, idname)) {
      return &mt;
    }
  }
  return nullptr;
}

const ListBaseT<ObjectModeType> &BKE_object_mode_types_get()
{
  return g_object_mode_types;
}

bool BKE_object_mode_type_poll_object(const ObjectModeType *mt, const Object *ob)
{
  BLI_assert(ob->type >= 0 && ob->type < 64);
  return (mt->object_type_mask & (uint64_t(1) << ob->type)) != 0;
}

void BKE_object_mode_types_exit()
{
  while (ObjectModeType *mt = static_cast<ObjectModeType *>(BLI_pophead(&g_object_mode_types))) {
    MEM_delete(mt);
  }
}

}  // namespace blender
