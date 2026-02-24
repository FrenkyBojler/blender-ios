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
  
  typedef struct CaptionsStripRef {
    struct CaptionsStripRef *next, *prev;
    Strip *strip = nullptr;
    char use_custom_style = 0;
  } CaptionsStripRef;

namespace seq {
    
void captions_update_strips(struct Scene *scene);
CaptionsStripRef *captions_get_ref_by_strip(struct Editing *ed, struct Strip *strip);
void captions_mark_ref_style_custom(CaptionsStripRef *ref, bool use_custom);
TextVars *captions_style_ensure(Editing *ed);

}  // namespace seq
}  // namespace blender