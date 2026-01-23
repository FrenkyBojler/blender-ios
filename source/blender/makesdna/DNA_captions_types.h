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

  // TODO: Those methods should be placed in a proper header, here just for quick testing
  void update_current_strips(struct Scene *scene, struct SpaceCaptions *scaptions);
  Strip *style_leader_strip_ensure(struct SpaceCaptions *scaptions);
  
}  // namespace blender