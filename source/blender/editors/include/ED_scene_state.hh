#pragma once

#include "DNA_scene_types.h"

/* Scene supports 4 transform orientation slots. */
#ifndef TRANSFORM_ORIENTATION_SLOT_MAX
#  define TRANSFORM_ORIENTATION_SLOT_MAX 4
#endif

typedef struct EditModeSceneState {
  /* Custom transform orientations. */
  ListBase transform_spaces;
  /* Active orientation slots. */
  TransformOrientationSlot orientation_slots[TRANSFORM_ORIENTATION_SLOT_MAX];

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
