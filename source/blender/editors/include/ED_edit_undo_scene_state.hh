/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "DNA_scene_types.h"

namespace blender::ed {

struct EditModeSceneState {
  /* Custom transform orientations. */
  ListBase transform_spaces;
  /* Active orientation slots. */
  TransformOrientationSlot orientation_slots[4];

  /* Proportional edit settings. */
  char proportional_edit;
  char prop_mode;
  float proportional_size;

  ~EditModeSceneState()
  {
    BLI_freelistN(&transform_spaces);
  }
};

/* Store and restore scene state for undo. */
void ED_scene_state_store(EditModeSceneState &state, const Scene &scene);
void ED_scene_state_restore(Scene &scene, const EditModeSceneState &state);

}  // namespace blender::ed
