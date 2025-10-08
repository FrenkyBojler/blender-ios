#include "ED_editmode_scene_state.hh"

#include <string.h>

#include "BLI_listbase.h"
#include "MEM_guardedalloc.h"
#include "DNA_scene_types.h"


static void transform_orientations_copy(ListBase *dst, const ListBase *src)
{
  BLI_freelistN(dst);
  BLI_duplicatelist(dst, src);
}

void ED_editmode_scene_state_init(EditModeSceneState *state)
{
  memset(state, 0, sizeof(*state));
  BLI_listbase_clear(&state->transform_spaces);
}

void ED_editmode_scene_state_free(EditModeSceneState *state)
{
  BLI_freelistN(&state->transform_spaces);
  BLI_listbase_clear(&state->transform_spaces);
}

void ED_editmode_scene_state_capture(EditModeSceneState *dst, const Scene *scene)
{
  transform_orientations_copy(&dst->transform_spaces, &scene->transform_spaces);

  memcpy(dst->orientation_slots,
         scene->orientation_slots,
         sizeof(dst->orientation_slots));

  const ToolSettings *ts = scene->toolsettings;
  if (ts) {
    dst->transform_pivot_point = ts->transform_pivot_point;

    dst->proportional_edit = ts->proportional_edit;
    dst->prop_mode = ts->prop_mode;
    dst->proportional_size = ts->proportional_size;

    dst->snap_mode = ts->snap_mode;
    dst->snap_flag = ts->snap_flag;
    dst->snap_target = ts->snap_target;
    dst->snap_transform_mode_flag = ts->snap_transform_mode_flag;

    dst->selectmode = ts->selectmode;

    dst->uv_selectmode = ts->uv_selectmode;
    dst->uv_sticky = ts->uv_sticky;
    dst->uv_flag = ts->uv_flag;
  }
  else {
    dst->transform_pivot_point = 0;
    dst->proportional_edit = 0;
    dst->prop_mode = 0;
    dst->proportional_size = 0.0f;
    dst->snap_mode = 0;
    dst->snap_flag = 0;
    dst->snap_target = 0;
    dst->snap_transform_mode_flag = 0;
    dst->selectmode = 0;
    dst->uv_selectmode = 0;
    dst->uv_sticky = 0;
    dst->uv_flag = 0;
  }
}

void ED_editmode_scene_state_restore(Scene *scene, const EditModeSceneState *src)
{
  transform_orientations_copy(&scene->transform_spaces, &src->transform_spaces);

  memcpy(scene->orientation_slots,
         src->orientation_slots,
         sizeof(src->orientation_slots));

  ToolSettings *ts = scene->toolsettings;
  if (!ts) {
    return;
  }

  ts->transform_pivot_point = src->transform_pivot_point;

  ts->proportional_edit = src->proportional_edit;
  ts->prop_mode = src->prop_mode;
  ts->proportional_size = src->proportional_size;

  ts->snap_mode = src->snap_mode;
  ts->snap_flag = src->snap_flag;
  ts->snap_target = src->snap_target;
  ts->snap_transform_mode_flag = src->snap_transform_mode_flag;

  ts->selectmode = src->selectmode;

  ts->uv_selectmode = src->uv_selectmode;
  ts->uv_sticky = src->uv_sticky;
  ts->uv_flag = src->uv_flag;
}
