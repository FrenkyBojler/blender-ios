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
  DEG_graph_free(this->staggered_eval.depsgraph);
}

void wm_staggered_eval_register(WindowRuntime &runtime,
                                ID &id,
                                const StringRef component_name,
                                const Bounds<int> range,
                                EvalCallback callback)
{
  StaggeredEvalData &eval_data = runtime.staggered_eval;
  for (StaggeredEvalTarget &eval_id : eval_data.targets) {
    if (eval_id.id_uid == id.session_uid && eval_id.component_name == component_name) {
      /* ID already in objects to evaluate. */
      eval_id.range = bounds::merge(eval_id.range, range);
      eval_data.range = {};
      return;
    }
  }

  eval_data.targets.append(
      {id.session_uid, GS(id.name), std::string(component_name), range, callback});

  /* Deleting the graph triggers a rebuild. */
  DEG_graph_free(eval_data.depsgraph);
  eval_data.depsgraph = nullptr;
  eval_data.range = {};
}

void wm_staggered_eval_prepare(Main &bmain, wmWindow &window)
{
  StaggeredEvalData &eval_data = window.runtime->staggered_eval;
  Vector<ID *> ids;
  Vector<int> invalid_id_indices;
  for (const int i : eval_data.targets.index_range()) {
    StaggeredEvalTarget &off_frame_id = eval_data.targets[i];
    /* Searching here means computationally this scales linear with the amount of `id_type` in the
     * file. This is not ideal performance wise, but doing it this way means we can react to the
     * object being deleted solely in this function without having to call a deregister function
     * anywhere. This leaves a small issue where X to delete may remove the object from the scene
     * without deleting it from Main. In that case this logic will fail to remove the ID from
     * the evaluation. However it will just run to completion which should create no problem. */
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
    eval_data.targets.remove(i);
    DEG_graph_free(eval_data.depsgraph);
    eval_data.depsgraph = nullptr;
  }

  if (eval_data.depsgraph && DEG_needs_update_relations(eval_data.depsgraph)) {
    DEG_graph_free(eval_data.depsgraph);
    eval_data.depsgraph = nullptr;
  }

  if (!eval_data.depsgraph) {
    eval_data.depsgraph = DEG_graph_new(&bmain,
                                        WM_window_get_active_scene(&window),
                                        WM_window_get_active_view_layer(&window),
                                        DAG_EVAL_VIEWPORT);
    DEG_graph_build_from_ids(eval_data.depsgraph, ids);
  }
}

static int get_next_frame(StaggeredEvalData &eval_data,
                          const Bounds<int> eval_range,
                          const int current_frame)
{
  int eval_frame;
  if (eval_data.range.is_empty()) {
    if (eval_range.contains(current_frame)) {
      eval_frame = current_frame;
    }
    else if (abs(eval_range.min - current_frame) < abs(eval_range.max - current_frame)) {
      eval_frame = eval_range.min;
    }
    else {
      eval_frame = eval_range.max;
    }
    eval_data.range = {current_frame, current_frame + 1};
  }
  else {
    if (abs(eval_data.range.min - current_frame) < abs(eval_data.range.max - current_frame)) {
      eval_frame = eval_data.range.min - 1;
      eval_data.range.min -= 1;
    }
    else {
      eval_frame = eval_data.range.max;
      eval_data.range.max += 1;
    }
  }
  return eval_frame;
}

bool wm_staggered_eval_next_frame(WindowRuntime &runtime, const int current_frame)
{
  StaggeredEvalData &eval_data = runtime.staggered_eval;
  if (eval_data.depsgraph == nullptr) {
    /* Call `wm_staggered_eval_prepare` before. */
    BLI_assert_unreachable();
    return false;
  }

  Bounds<int> eval_range = {};
  for (const int i : eval_data.targets.index_range()) {
    StaggeredEvalTarget &off_frame_id = eval_data.targets[i];
    /* `wm_staggered_eval_prepare` has to be called before. */
    BLI_assert(off_frame_id.id != nullptr);
    eval_range = bounds::merge(eval_range, off_frame_id.range);
  }

  if (eval_range.is_empty()) {
    return false;
  }

  const int eval_frame = get_next_frame(eval_data, eval_range, current_frame);
  DEG_evaluate_on_framechange(eval_data.depsgraph, eval_frame);

  Vector<int> finished_indices;
  for (const int i : eval_data.targets.index_range()) {
    StaggeredEvalTarget &off_frame_id = eval_data.targets[i];
    ID *eval_id = DEG_get_evaluated_id(eval_data.depsgraph, off_frame_id.id);
    /* The callback shall return true when the evaluation has completed. */
    if (off_frame_id.callback(*eval_data.depsgraph,
                              *off_frame_id.id,
                              *eval_id,
                              off_frame_id.component_name,
                              eval_frame))
    {
      finished_indices.append(i);
    }
  }

  bool can_eval_without_prepare = true;
  while (!finished_indices.is_empty()) {
    const int i = finished_indices.pop_last();
    eval_data.targets.remove(i);
    DEG_graph_free(eval_data.depsgraph);
    eval_data.depsgraph = nullptr;
    /* There may still be IDs to evaluate, however we have to rebuild the depsgraph so we signal
     * the caller that for now there is nothing more to evaluate and we will wait for the next
     * iteration. */
    can_eval_without_prepare = false;
  }
  return can_eval_without_prepare;
}

}  // namespace blender::bke
