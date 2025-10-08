#include "ED_scene_state.hh"

#include <string.h>

#include "BLI_listbase.h"
#include "DNA_scene_types.h"
#include "MEM_guardedalloc.h"

void ED_scene_state_free(EditModeSceneState *state)
{
  BLI_freelistN(&state->transform_spaces);
  BLI_listbase_clear(&state->transform_spaces);
}

static void transform_orientations_copy(ListBase *dst, const ListBase *src)
{
  BLI_freelistN(dst);
  BLI_duplicatelist(dst, src);
}

/* Capture current scene settings used in edit mode. */
void ED_scene_state_capture(EditModeSceneState *dst, const Scene *scene)
{
  transform_orientations_copy(&dst->transform_spaces, &scene->transform_spaces);
  memcpy(dst->orientation_slots, scene->orientation_slots, sizeof(dst->orientation_slots));

  const ToolSettings *ts = scene->toolsettings;
  if (ts) {
    dst->proportional_edit = ts->proportional_edit;
    dst->prop_mode = ts->prop_mode;
    dst->proportional_size = ts->proportional_size;
  }
  else {
    dst->proportional_edit = 0;
    dst->prop_mode = 0;
    dst->proportional_size = 0.0f;
  }
}

/* Restore captured scene settings after undo. */
void ED_scene_state_restore(Scene *scene, const EditModeSceneState *src)
{
  transform_orientations_copy(&scene->transform_spaces, &src->transform_spaces);
  memcpy(scene->orientation_slots, src->orientation_slots, sizeof(src->orientation_slots));

  ToolSettings *ts = scene->toolsettings;
  if (!ts) {
    return;
  }

  ts->proportional_edit = src->proportional_edit;
  ts->prop_mode = src->prop_mode;
  ts->proportional_size = src->proportional_size;
}
