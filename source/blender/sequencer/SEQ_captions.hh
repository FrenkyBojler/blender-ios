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
    
void captions_cache_rebuild(struct Scene *scene);
void captions_apply_style_single(Scene *scene, SeqTimelineChannel *channel, Strip *strip);
void captions_apply_style_active(Scene *scene);
void captions_update_active(Scene *scene);
void captions_set_style_custom(Strip *strip, bool use_custom);
const Vector<Strip *> captions_cache_query(Scene *scene);
Strip *captions_cache_query_index(Scene *scene, int index);

// TODO: GD;; Convert all cache dirty setter to this method
void captions_cache_mark_dirty(Scene *scene);

/** Pass nullptr as default, which will default to the first channel */
void captions_active_channel_set(Editing *ed, SeqTimelineChannel *channel=nullptr);

}  // namespace seq
}  // namespace blender