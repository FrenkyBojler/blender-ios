/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.h"
#include "ED_anim_api.hh"
#include "ED_space_action.hh"

namespace blender::ed::action {

void get_visible_elements(bContext *C, ListBase &r_anim_data)
{
  bAnimContext ac;
  if (!ANIM_animdata_get_context(C, &ac)) {
    return;
  }

  const eAnimFilter_Flags filter = ANIMFILTER_DATA_VISIBLE;
  size_t size = ANIM_animdata_filter(&ac, &r_anim_data, filter, ac.data, ac.datatype);
}

}  // namespace blender::ed::action
