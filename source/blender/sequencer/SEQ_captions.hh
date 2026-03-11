/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include "DNA_listBase.h"
#include "DNA_defs.h"
#include "DNA_listBase.h"

namespace blender {
    
  struct Strip;
  struct Editing;
  struct TextVars;
  struct CaptionsChannelData;

namespace seq {
    
CaptionsChannelData *captions_active_ensure(Editing *ed);
CaptionsChannelData *captions_active_get(Editing *ed);
void captions_update_active(struct Scene *scene);
void captions_apply_style_single(CaptionsChannelData *captions_data, Scene *scene, Caption *caption);
void captions_apply_style_active(Scene *scene);
void captions_mark_caption_style_custom(Caption *caption, bool use_custom);
Caption *captions_get_single_by_strip(CaptionsChannelData *captions_data, struct Strip *strip);
Caption *captions_get_single_by_index(CaptionsChannelData *captions_data, int index);

/** Pass nullptr as default, which will default to the first channel */
void captions_set_active_channel(Editing *ed, SeqTimelineChannel *channel=nullptr);

}  // namespace seq
}  // namespace blender