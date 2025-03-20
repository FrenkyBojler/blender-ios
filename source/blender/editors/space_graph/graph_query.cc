/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.h"
#include "ED_anim_api.hh"
#include "ED_space_graph.hh"

namespace blender::ed::graph {

blender::Vector<bAnimListElem *> get_editable_fcurves(bContext *C)
{
  bAnimContext ac;
  /* Get editor data. */
  if (ANIM_animdata_get_context(C, &ac) == 0) {
    return {};
  }

  ListBase anim_data = {nullptr, nullptr};
  int filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_CURVE_VISIBLE | ANIMFILTER_FCURVESONLY |
                ANIMFILTER_NODUPLIS);
  if (U.animation_flag & USER_ANIM_ONLY_SHOW_SELECTED_CURVE_KEYS) {
    filter |= ANIMFILTER_SEL;
  }

  size_t size = ANIM_animdata_filter(
      &ac, &anim_data, eAnimFilter_Flags(filter), ac.data, eAnimCont_Types(ac.datatype));
  blender::Vector<bAnimListElem *> fcurves(size);
  int i;
  LISTBASE_FOREACH_INDEX (bAnimListElem *, ale, &anim_data, i) {
    BLI_assert(ale->type == ANIMTYPE_FCURVE);
    fcurves[i] = ale;
  }
  ANIM_animdata_freelist(&anim_data);

  return fcurves;
}

}  // namespace blender::ed::graph
