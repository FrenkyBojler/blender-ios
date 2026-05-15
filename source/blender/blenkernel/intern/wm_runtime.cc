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
  runtime.rebuild_async_depsgraph = true;
  runtime.evaluated_range = {};
}

void wm_runtime_evaluate_next_frame(Main &bmain, WindowRuntime &runtime, const Scene &scene)
{
  if (runtime.async_depsgraph == nullptr) {
    /* Depsgraph should be built before. */
    BLI_assert_unreachable();
    return;
  }

  Vector<ID *> ids;
  Bounds<int> eval_range = {};
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
      continue;
    }
    off_frame_id.id = id;
    ids.append(id);
    eval_range = bounds::merge(eval_range, off_frame_id.range);
  }

  while (!invalid_id_indices.is_empty()) {
    const int i = invalid_id_indices.pop_last();
    runtime.async_eval_ids.remove(i);
    runtime.rebuild_async_depsgraph = true;
  }

  if (eval_range.is_empty()) {
    return;
  }

  if (runtime.rebuild_async_depsgraph) {
    DEG_graph_build_from_ids(runtime.async_depsgraph, ids);
    runtime.rebuild_async_depsgraph = false;
  }

  const int cfra = BKE_scene_frame_get(&scene);
  int eval_frame;
  if (runtime.evaluated_range.is_empty()) {
    if (eval_range.contains(cfra)) {
      eval_frame = cfra;
    }
    else if (abs(eval_range.min - cfra) < abs(eval_range.max - cfra)) {
      eval_frame = eval_range.min;
    }
    else {
      eval_frame = eval_range.max;
    }
    runtime.evaluated_range = {cfra, cfra + 1};
  }
  else {
    if (abs(runtime.evaluated_range.min - cfra) < abs(runtime.evaluated_range.max - cfra)) {
      eval_frame = runtime.evaluated_range.min - 1;
      runtime.evaluated_range.min -= 1;
    }
    else {
      eval_frame = runtime.evaluated_range.max;
      runtime.evaluated_range.max += 1;
    }
  }

  /* const Clock::time_point start = Clock::now();
  while (Clock::now() - start < std::chrono::milliseconds(16)) {
  } */
  DEG_evaluate_on_framechange(runtime.async_depsgraph, eval_frame);

  Vector<int> finished_indices;
  for (const int i : runtime.async_eval_ids.index_range()) {
    bke::AsyncEvalId &off_frame_id = runtime.async_eval_ids[i];
    ID *eval_id = DEG_get_evaluated_id(runtime.async_depsgraph, off_frame_id.id);
    if (!off_frame_id.range.contains(eval_frame)) {
      continue;
    }
    /* The callback shall return true when the evaluation has completed. */
    if (off_frame_id.callback(*off_frame_id.id, *eval_id, off_frame_id.component_name, eval_frame))
    {
      finished_indices.append(i);
    }
  }

  while (!finished_indices.is_empty()) {
    const int i = finished_indices.pop_last();
    runtime.async_eval_ids.remove(i);
    runtime.rebuild_async_depsgraph = true;
  }
}

}  // namespace blender::bke
