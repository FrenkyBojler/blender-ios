/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edundo
 *
 * The `CUSTOM_MODE` undo type — the C wrapper that lets an addon-registered
 * object mode (#OB_MODE_CUSTOM) provide delta undo instead of full memfile
 * snapshots. The heavy per-step data lives inside the addon/engine; a Blender
 * undo step stores only an integer state id (the addon's key) plus a truthful
 * byte size, and applies the delta through the mode's `undo_decode`/`undo_free`
 * callbacks.
 *
 * The type is opt-in: `poll` is true only for an active object in a custom
 * mode whose #ObjectModeType set #OBJECT_MODE_TYPE_USE_CUSTOM_UNDO and defined
 * the callbacks, so modes that stay on memfile undo are unaffected.
 *
 * This is the Tier-2 lifecycle (push / decode / free); the full memfile-
 * interleave discipline (undo-integration plan §4) layers on top.
 */

#include "MEM_guardedalloc.h"

#include "BLI_string_utf8.hh"
#include "BLI_utildefines.hh"

#include "DNA_object_enums.h"
#include "DNA_object_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_object_modes.hh"
#include "BKE_scene.hh"
#include "BKE_undo_system.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_undo.hh"

namespace blender::ed {

struct CustomModeUndoStep {
  UndoStep step;
  /** The object the stroke acted on (name-tracked across rename/undo). */
  UndoRefID_Object object_ref;
  /** Idname of the mode active at push (resolves the callback owner). */
  char mode_idname[64];
  /** The addon's opaque state key for this step. */
  int state_id;
};

/* Set by ED_custom_mode_undo_push just before the push; read by step_encode
 * (the push machinery has no channel for extra per-step payload). */
static int g_pending_state_id = 0;
static size_t g_pending_size = 0;

static Object *custom_mode_active_object(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  return BKE_view_layer_active_object_get(view_layer);
}

static bool custom_mode_undosys_poll(bContext *C)
{
  return BKE_object_custom_mode_uses_custom_undo(custom_mode_active_object(C));
}

static bool custom_mode_undosys_step_encode(bContext *C, Main * /*bmain*/, UndoStep *us_p)
{
  CustomModeUndoStep *us = reinterpret_cast<CustomModeUndoStep *>(us_p);
  Object *ob = custom_mode_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  us->object_ref.ptr = ob;
  STRNCPY_UTF8(us->mode_idname, ob->custom_mode_id);
  us->state_id = g_pending_state_id;
  us->step.data_size = g_pending_size;
  return true;
}

static void custom_mode_undosys_step_decode(
    bContext *C, Main * /*bmain*/, UndoStep *us_p, const eUndoStepDir dir, bool /*is_final*/)
{
  CustomModeUndoStep *us = reinterpret_cast<CustomModeUndoStep *>(us_p);
  Object *ob = us->object_ref.ptr;
  if (ob == nullptr) {
    return;
  }
  ObjectModeType *mt = BKE_object_mode_type_find(us->mode_idname);
  if (mt == nullptr || mt->undo_decode == nullptr) {
    return;
  }
  /* Only apply the engine delta when the object is still in this mode (its
   * session is alive). If a foreign memfile step in between rebuilt Main, the
   * mode's `refresh` resyncs from the Mesh ID and the delta is a no-op here
   * (undo-integration §4). */
  if ((ob->mode & OB_MODE_CUSTOM) == 0 || !STREQ(ob->custom_mode_id, us->mode_idname)) {
    return;
  }
  mt->undo_decode(mt, C, ob, us->state_id, dir == STEP_UNDO ? -1 : 1);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob->id);
}

static void custom_mode_undosys_step_free(UndoStep *us_p)
{
  CustomModeUndoStep *us = reinterpret_cast<CustomModeUndoStep *>(us_p);
  ObjectModeType *mt = BKE_object_mode_type_find(us->mode_idname);
  if (mt != nullptr && mt->undo_free != nullptr) {
    mt->undo_free(mt, us->state_id);
  }
}

static void custom_mode_undosys_foreach_ID_ref(UndoStep *us_p,
                                               UndoTypeForEachIDRefFn foreach_ID_ref_fn,
                                               void *user_data)
{
  CustomModeUndoStep *us = reinterpret_cast<CustomModeUndoStep *>(us_p);
  foreach_ID_ref_fn(user_data, reinterpret_cast<UndoRefID *>(&us->object_ref));
}

void ED_custom_mode_undosys_type(UndoType *ut)
{
  ut->identifier = "CUSTOM_MODE";
  ut->poll = custom_mode_undosys_poll;
  ut->step_encode = custom_mode_undosys_step_encode;
  ut->step_decode = custom_mode_undosys_step_decode;
  ut->step_free = custom_mode_undosys_step_free;
  ut->step_foreach_ID_ref = custom_mode_undosys_foreach_ID_ref;
  ut->flags = UNDOTYPE_FLAG_NEED_CONTEXT_FOR_ENCODE;
  ut->step_size = sizeof(CustomModeUndoStep);
}

void ED_custom_mode_undo_push(bContext *C, const char *name, const int state_id, const size_t size)
{
  UndoStack *ustack = ED_undo_stack_get();
  if (ustack == nullptr) {
    return;
  }
  g_pending_state_id = state_id;
  g_pending_size = size;
  BKE_undosys_step_push_with_type(ustack, C, name, BKE_UNDOSYS_TYPE_CUSTOM_MODE);
}

}  // namespace blender::ed
