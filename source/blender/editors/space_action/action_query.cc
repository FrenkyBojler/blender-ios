/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.h"
#include "ED_anim_api.hh"
#include "ED_space_action.hh"

namespace blender::ed::action {

blender::Vector<bAnimListElem *> get_visible_elements(bContext *C)
{
  bAnimContext ac;
  if (!ANIM_animdata_get_context(C, &ac)) {
    return {};
  }
  ListBase anim_data = {nullptr, nullptr};
  const eAnimFilter_Flags filter = ANIMFILTER_DATA_VISIBLE | ANIMFILTER_FCURVESONLY;
  size_t size = ANIM_animdata_filter(&ac, &anim_data, filter, ac.data, ac.datatype);

  blender::Vector<bAnimListElem *> anim_elements(size);
  int i;
  LISTBASE_FOREACH_INDEX (bAnimListElem *, ale, &anim_data, i) {
    anim_elements[i] = ale;
  }

  ANIM_animdata_freelist(&anim_data);
  return anim_elements;
}

}  // namespace blender::ed::action
