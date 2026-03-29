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

namespace seq {
    
void captions_apply_style_single(Scene *scene, SeqTimelineChannel *channel, Strip *strip);
void captions_apply_style_active(Scene *scene);
void captions_update_active(Scene *scene);
void captions_set_style_custom(Strip *strip, bool use_custom);
const Vector<Strip *> caption_strips_query(Scene *scene);
Strip *caption_strips_query_index(Scene *scene, int index);

// TODO: GD;; Maybe move the whole cache system into the sequencer_intern header?
void caption_strips_sort(Scene *scene);
void caption_strips_append(Scene *scene, Strip *strip);
void caption_strips_remove(Scene *scene, Strip *strip);
void caption_strips_rebuild(struct Scene *scene);

/** Pass nullptr as default, which will default to the first channel */
void captions_active_channel_set(Editing *ed, SeqTimelineChannel *channel=nullptr);

}  // namespace seq
}  // namespace blender