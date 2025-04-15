/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.h"
#include "ED_anim_api.hh"
#include "ED_space_graph.hh"

namespace blender::ed::graph {

void get_editable_fcurves(bAnimContext &ac, ListBase &r_anim_data)
{
  eAnimFilter_Flags filter = (ANIMFILTER_DATA_VISIBLE | ANIMFILTER_CURVE_VISIBLE |
                              ANIMFILTER_FCURVESONLY | ANIMFILTER_NODUPLIS);
  if (U.animation_flag & USER_ANIM_ONLY_SHOW_SELECTED_CURVE_KEYS) {
    filter |= ANIMFILTER_SEL;
  }

  ANIM_animdata_filter(&ac, &r_anim_data, filter, ac.data, eAnimCont_Types(ac.datatype));
}

}  // namespace blender::ed::graph
