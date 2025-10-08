#pragma once

#include "DNA_scene_types.h"

#ifndef TRANSFORM_ORIENTATION_SLOT_MAX
#  define TRANSFORM_ORIENTATION_SLOT_MAX 4
#endif

typedef struct EditModeSceneState {
  ListBase transform_spaces;

   TransformOrientationSlot orientation_slots[TRANSFORM_ORIENTATION_SLOT_MAX];

  char transform_pivot_point;
  char proportional_edit;
  char prop_mode;
  float proportional_size;

  short snap_mode;
  short snap_flag;
  char  snap_target;
  char  snap_transform_mode_flag;

  char selectmode;

  char uv_selectmode;
  char uv_flag;
  char uv_sticky;
} EditModeSceneState;

void ED_editmode_scene_state_init(EditModeSceneState *state);
void ED_editmode_scene_state_free(EditModeSceneState *state);

void ED_editmode_scene_state_capture(EditModeSceneState *state, const Scene *scene);
void ED_editmode_scene_state_restore(Scene *scene, const EditModeSceneState *state);


