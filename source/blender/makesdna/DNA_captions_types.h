/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_defs.h"
#include "DNA_listBase.h"

namespace blender {
    
  struct Strip;
  
  typedef struct CaptionsStripRef {
    struct CaptionsStripRef *next, *prev;
    Strip *strip = nullptr;
    char use_custom_style = 0;
  } CaptionsStripRef;

  // TODO: Those methods should be placed in a proper header, here just for quick testing, It'll stay here until the exact location of the captions in the UI is decided.
  void update_current_strips(struct Scene *scene, struct SpaceCaptions *scaptions);
  Strip *style_leader_strip_ensure(struct SpaceCaptions *scaptions);
  CaptionsStripRef *get_ref_by_strip(struct SpaceCaptions *scaptions, struct Strip *strip);
  void mark_ref_style_custom(CaptionsStripRef *ref, bool use_custom);
  
}  // namespace blender