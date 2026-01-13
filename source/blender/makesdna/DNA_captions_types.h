/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_defs.h"
#include "DNA_listBase.h"
#include "DNA_sequence_types.h"  // For Strip

void update_current_strips(struct Scene *scene, struct SpaceCaptions *scaptions);

typedef struct CaptionsStripRef {
  struct CaptionsStripRef *next, *prev;
  Strip *strip;
} CaptionsStripRef;