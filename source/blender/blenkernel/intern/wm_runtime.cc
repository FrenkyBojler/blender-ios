/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_undo_system.hh"
#include "BKE_wm_runtime.hh"

#include "BLI_bounds.hh"
#include "BLI_ghash.h"
#include "BLI_listbase.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "WM_api.hh"

#include "WM_message.hh"

namespace blender::bke {

WindowManagerRuntime::WindowManagerRuntime()
{
  BKE_reports_init(&this->reports, RPT_STORE);
}

WindowManagerRuntime::~WindowManagerRuntime()
{
  BKE_reports_free(&this->reports);

  BLI_freelistN(&this->notifier_queue);

  while (wmOperator *op = static_cast<wmOperator *>(BLI_pophead(&this->operators))) {
    WM_operator_free(op);
  }

  BLI_freelistN(&this->paintcursors);

  /* NOTE(@ideasman42): typically timers are associated with windows and timers will have been
   * freed when the windows are removed. However timers can be created which don't have windows
   * and in this case it's necessary to free them on exit, see: #109953. */
  while (wmTimer *timer = static_cast<wmTimer *>(BLI_pophead(&this->timers))) {
    WM_event_timer_free_data(timer);
    MEM_delete(timer);
  }

  while (wmKeyConfig *keyconf = static_cast<wmKeyConfig *>(BLI_pophead(&this->keyconfigs))) {
    WM_keyconfig_free(keyconf);
  }

  WM_drag_free_list(&this->drags);

  if (this->undo_stack) {
    BKE_undosys_stack_destroy(this->undo_stack);
  }

  if (this->message_bus != nullptr) {
    WM_msgbus_destroy(this->message_bus);
  }
}

WindowRuntime::~WindowRuntime()
{
#ifdef WITH_INPUT_IME
  BLI_assert(this->ime_data == nullptr);
#endif
  /** The event_queue should be freed when the window is freed. */
  BLI_assert(BLI_listbase_is_empty(&this->event_queue));
  DEG_graph_free(this->async_depsgraph);
}

void wm_runtime_range_eval_register(WindowRuntime &runtime,
                                    ID &id,
                                    const StringRef component_name,
                                    const Bounds<int> range,
                                    EvalCallback callback)
{
  for (AsyncEvalId &eval_id : runtime.async_eval_ids) {
    if (eval_id.id_uid == id.session_uid && eval_id.component_name == component_name) {
      /* ID already in objects to evaluate. */
      eval_id.range = bounds::merge(eval_id.range, range);
      runtime.evaluated_range = {};
      return;
    }
  }
  runtime.async_eval_ids.append(
      {id.session_uid, GS(id.name), std::string(component_name), range, callback});

  /* Deleting the graph triggers a rebuild. */
  DEG_graph_free(runtime.async_depsgraph);
  runtime.async_depsgraph = nullptr;
  runtime.evaluated_range = {};
}

void wm_runtime_prepare_for_eval(Main &bmain, wmWindow &window)
{
  WindowRuntime &runtime = *window.runtime;
  Vector<ID *> ids;
  Vector<int> invalid_id_indices;
  for (const int i : runtime.async_eval_ids.index_range()) {
    bke::AsyncEvalId &off_frame_id = runtime.async_eval_ids[i];
    /* Searching here means computationally this scales linear with the amount of `id_type` in the
     * file. This is not ideal performance wise, but doing it this way means we can react to the
     * object being deleted solely in this function without having to call a deregister function
     * anywhere. */
    ID *id = BKE_libblock_find_session_uid(&bmain, off_frame_id.id_type, off_frame_id.id_uid);
    if (!id) {
      invalid_id_indices.append(i);
      off_frame_id.id = nullptr;
      continue;
    }
    ids.append(id);
    off_frame_id.id = id;
  }

  while (!invalid_id_indices.is_empty()) {
    const int i = invalid_id_indices.pop_last();
    runtime.async_eval_ids.remove(i);
    DEG_graph_free(runtime.async_depsgraph);
    runtime.async_depsgraph = nullptr;
  }

  if (!runtime.async_depsgraph) {
    runtime.async_depsgraph = DEG_graph_new(&bmain,
                                            WM_window_get_active_scene(&window),
                                            WM_window_get_active_view_layer(&window),
                                            DAG_EVAL_VIEWPORT);
    DEG_graph_build_from_ids(runtime.async_depsgraph, ids);
  }
}

static int get_next_frame(WindowRuntime &runtime,
                          const Bounds<int> eval_range,
                          const int current_frame)
{
  int eval_frame;
  if (runtime.evaluated_range.is_empty()) {
    if (eval_range.contains(current_frame)) {
      eval_frame = current_frame;
    }
    else if (abs(eval_range.min - current_frame) < abs(eval_range.max - current_frame)) {
      eval_frame = eval_range.min;
    }
    else {
      eval_frame = eval_range.max;
    }
    runtime.evaluated_range = {current_frame, current_frame + 1};
  }
  else {
    if (abs(runtime.evaluated_range.min - current_frame) <
        abs(runtime.evaluated_range.max - current_frame))
    {
      eval_frame = runtime.evaluated_range.min - 1;
      runtime.evaluated_range.min -= 1;
    }
    else {
      eval_frame = runtime.evaluated_range.max;
      runtime.evaluated_range.max += 1;
    }
  }
  return eval_frame;
}

bool wm_runtime_evaluate_next_frame(WindowRuntime &runtime, const int current_frame)
{
  if (runtime.async_depsgraph == nullptr) {
    /* Call `wm_runtime_prepare_for_eval` before. */
    BLI_assert_unreachable();
    return false;
  }

  Bounds<int> eval_range = {};
  for (const int i : runtime.async_eval_ids.index_range()) {
    bke::AsyncEvalId &off_frame_id = runtime.async_eval_ids[i];
    /* `wm_runtime_prepare_for_eval` has to be called before. */
    BLI_assert(off_frame_id.id != nullptr);
    eval_range = bounds::merge(eval_range, off_frame_id.range);
  }

  if (eval_range.is_empty()) {
    return false;
  }

  const int eval_frame = get_next_frame(runtime, eval_range, current_frame);
  DEG_evaluate_on_framechange(runtime.async_depsgraph, eval_frame);

  Vector<int> finished_indices;
  for (const int i : runtime.async_eval_ids.index_range()) {
    bke::AsyncEvalId &off_frame_id = runtime.async_eval_ids[i];
    ID *eval_id = DEG_get_evaluated_id(runtime.async_depsgraph, off_frame_id.id);
    /* The callback shall return true when the evaluation has completed. */
    if (off_frame_id.callback(*off_frame_id.id, *eval_id, off_frame_id.component_name, eval_frame))
    {
      finished_indices.append(i);
    }
  }

  while (!finished_indices.is_empty()) {
    const int i = finished_indices.pop_last();
    runtime.async_eval_ids.remove(i);
    DEG_graph_free(runtime.async_depsgraph);
    runtime.async_depsgraph = nullptr;
    /* There may still be IDs to evaluate, however we have to rebuild the depsgraph so we signal
     * the caller that for now there is nothing more to evaluate and we will wait for the next
     * iteration. */
    return false;
  }
  return true;
}

}  // namespace blender::bke
