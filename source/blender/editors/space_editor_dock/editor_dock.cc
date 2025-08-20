/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup speditordock
 */

#include "BKE_screen.hh"

#include "BLI_listbase.h"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_editor_dock.hh"

namespace blender::ed::editor_dock {

/* TODO this isn't editor dock specific. Move somewhere else? */
void add_docked_space(ScrArea *area, const eSpace_Type type, const Scene *scene)
{
  SpaceType *st = BKE_spacetype_from_id(type);
  if (!st) {
    return;
  }

  SpaceLink *sl_old = static_cast<SpaceLink *>(area->spacedata.first);

  area->spacetype = type;
  area->type = st;

  SpaceLink *sl = st->create(area, scene);
  BLI_addhead(&area->spacedata, sl);

  if (sl_old) {
    sl_old->regionbase = area->regionbase;
  }
  area->regionbase = sl->regionbase;
  BLI_listbase_clear(&sl->regionbase);
}

}  // namespace blender::ed::editor_dock
