/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2003-2009 Blender Authors
 * SPDX-FileCopyrightText: 2005-2006 Peter Schlaile <peter [at] schlaile [dot] de>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_context.hh"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "SEQ_utils.hh"
#include "WM_api.hh"

#include "BLI_listbase.h"

#include "SEQ_select.hh"
#include "SEQ_sequencer.hh"

namespace blender::seq {

Strip *select_get_active_from_context(bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  const Editing *ed = seq::editing_get(scene);

  Strip *strip = select_active_get(scene);

  const wmWindow *win = CTX_wm_window(C);
  const bScreen *screen = WM_window_get_active_screen(win);
  const ARegion *region = screen->active_region;

  /* If we're in the N-panel, the active strip is always in context
   * (eg. not hidden inside a meta strip). */
  if (region && region->regiontype == RGN_TYPE_UI) {
    return strip;
  }

  /* In the case of the timeline. */
  ListBase *seqbase = seq::get_seqbase_by_strip(scene, strip);
  if (seqbase == seq::active_seqbase_get(ed)) {
    return strip;
  }

  return nullptr;
}

Strip *select_active_get(const Scene *scene)
{
  const Editing *ed = editing_get(scene);

  if (ed == nullptr) {
    return nullptr;
  }

  return ed->act_strip;
}

void select_active_set(Scene *scene, Strip *strip)
{
  Editing *ed = editing_get(scene);

  if (ed == nullptr) {
    return;
  }

  ed->act_strip = strip;
}

bool select_active_get_pair_from_context(bContext *C, Strip **r_strip_act, Strip **r_strip_other)
{
  const Scene *scene = CTX_data_scene(C);
  Editing *ed = editing_get(scene);

  *r_strip_act = select_get_active_from_context(C);

  if (*r_strip_act == nullptr) {
    return false;
  }

  *r_strip_other = nullptr;

  LISTBASE_FOREACH (Strip *, strip, ed->seqbasep) {
    if (strip->flag & SELECT && (strip != (*r_strip_act))) {
      if (*r_strip_other) {
        return false;
      }

      *r_strip_other = strip;
    }
  }

  return (*r_strip_other != nullptr);
}

}  // namespace blender::seq
