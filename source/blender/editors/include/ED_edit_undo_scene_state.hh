/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "DNA_scene_types.h"

typedef struct EditModeSceneState {
  /* Custom transform orientations. */
  ListBase transform_spaces;
  /* Active orientation slots. */
  TransformOrientationSlot orientation_slots[4];

  /* Proportional edit settings. */
  char proportional_edit;
  char prop_mode;
  float proportional_size;
} EditModeSceneState;

/* Lifecycle */
void ED_scene_state_free(EditModeSceneState *state);

/* Capture and restore scene state for undo. */
void ED_scene_state_capture(EditModeSceneState *state, const Scene *scene);
void ED_scene_state_restore(Scene *scene, const EditModeSceneState *state);
