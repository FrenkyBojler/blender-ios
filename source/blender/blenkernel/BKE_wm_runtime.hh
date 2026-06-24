/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BKE_report.hh"

#include "DNA_windowmanager_types.h"

#include "BLI_bounds_types.hh"
#include "BLI_function_ref.hh"
#include "BLI_set.hh"

namespace blender {

struct UndoStack;
struct wmMsgBus;
struct wmKeyConfig;
struct wmEvent;
struct wmWindow;
struct wmIMEData;
struct wmGesture;
struct wmJob;
struct wmDrag;
struct wmPaintCursor;
struct WindowDrawCB;
struct Main;
struct Depsgraph;

namespace bke {

struct wmNotifierHashForQueue {
  uint64_t operator()(const wmNotifier *note) const;
};
struct wmNotifierEqForQueue {
  bool operator()(const wmNotifier *a, const wmNotifier *b) const;
};
using wmNotifierQueueSet = Set<const wmNotifier *,
                               4,
                               DefaultProbingStrategy,
                               wmNotifierHashForQueue,
                               wmNotifierEqForQueue>;

struct WindowManagerRuntime {
  /** Separate active from drawable. */
  wmWindow *windrawable = nullptr;
  /**
   * \note `CTX_wm_window(C)` is usually preferred.
   * Avoid relying on this where possible as this may become NULL during when handling
   * events that close or replace windows (e.g. opening a file).
   * While this happens rarely in practice, it can cause difficult to reproduce bugs.
   */
  wmWindow *winactive = nullptr;

  /** Indicates whether interface is locked for user interaction. */
  bool is_interface_locked = false;

  /** Indicates whether modified images should be saved when saving the blend file. */
  char save_modified_images_when_file_is_saved = true;

  /**
   * Indicates the main loop (#WM_main()) to stop processing the event queue and move to the next
   * step. The Remaining events will then be processed during the next iteration of the loop.
   *
   * This is used e.g. to avoid handling events immediately after an undo/redo action, when UI has
   * not yet been updated.
   */
  bool break_events_handling = false;

  /** Information and error reports. */
  ReportList reports;

  /**
   * Refresh/redraw #wmNotifier structs.
   * \note Once in the queue, notifiers should be considered read-only.
   * With the exception of clearing notifiers for data which has been removed,
   * see: #NOTE_CATEGORY_TAG_CLEARED.
   */
  ListBaseT<wmNotifier> notifier_queue = {nullptr, nullptr};
  /**
   * For duplicate detection.
   * \note keep in sync with `notifier_queue` adding/removing elements must also update this set.
   */
  wmNotifierQueueSet notifier_queue_set;

  /** The current notifier in the `notifier_queue` being handled (clear instead of freeing). */
  const wmNotifier *notifier_current = nullptr;

  /** Operator registry. */
  ListBaseT<wmOperator> operators = {nullptr, nullptr};

  /** Extra overlay cursors to draw, like circles. */
  ListBaseT<wmPaintCursor> paintcursors = {nullptr, nullptr};

  /**
   * Known key configurations.
   * This includes all the #wmKeyConfig members (`defaultconf`, `addonconf`, etc).
   */
  ListBaseT<wmKeyConfig> keyconfigs = {nullptr, nullptr};

  /** Active timers. */
  ListBaseT<wmTimer> timers = {nullptr, nullptr};

  /** Threaded jobs manager. */
  ListBaseT<wmJob> jobs = {nullptr, nullptr};

  /** Active dragged items. */
  ListBaseT<wmDrag> drags = {nullptr, nullptr};

  /** Default configuration. */
  wmKeyConfig *defaultconf = nullptr;

  /** Addon configuration. */
  wmKeyConfig *addonconf = nullptr;

  /** User configuration. */
  wmKeyConfig *userconf = nullptr;

  /**
   * All undo history.
   *
   * \note This will be null in background mode unless explicitly created.
   */
  UndoStack *undo_stack = nullptr;

  wmMsgBus *message_bus = nullptr;

  WindowManagerRuntime();
  ~WindowManagerRuntime();
};

using EvalCallback = FunctionRef<bool(
    Depsgraph &dg, ID &orig_id, ID &evaluated_id, StringRef component_name, int frame)>;

struct StaggeredEvalTarget {
  /* Storing the session uid instead of a pointer so we can react to deletions in the evaluation
   * code. Doing so means we have to search for the `ID*`. */
  uint32_t id_uid;
  ID_Type id_type = ID_OB;
  /* Storing the name for a component of the ID. This is passed to the `callback`, but not used for
   * evaluation.  */
  std::string component_name;
  /* The range to evaluate for this ID. */
  Bounds<int> range;
  EvalCallback callback;

  /* Cached pointer to the ID based on `id_uid` and `id_type`. This is only valid after calling
   * `wm_staggered_eval_prepare`. */
  ID *id;
};

struct StaggeredEvalData {
  /**
   * A dependency graph used for evaluating the motion path objects of the current scene.
   * This depsgraph is a minimal version that only includes the motion path objects.
   */
  struct Depsgraph *depsgraph = nullptr;
  Vector<StaggeredEvalTarget> targets = {};
  /** Range of frames already evaluated. */
  Bounds<int> range = {};
};

struct WindowRuntime {
  /** All events #wmEvent (ghost level events were handled). */
  ListBaseT<wmEvent> event_queue = {nullptr, nullptr};

  /**
   * Input Method Editor data - complex character input (especially for Asian character input)
   * Only used when `WITH_INPUT_IME` is defined.
   */
  wmIMEData *ime_data = nullptr;
  bool ime_data_is_composing = false;

  /** Don't want to include ghost.h stuff. */
  void *ghostwin = nullptr;

  /** Don't want to include gpu stuff. */
  void *gpuctx = nullptr;

  /** Window+screen handlers, handled last. */
  ListBaseT<wmEventHandler> handlers = {nullptr, nullptr};

  /** Priority handlers, handled first. */
  ListBaseT<wmEventHandler> modalhandlers = {nullptr, nullptr};

  /** Custom drawing callbacks. */
  ListBaseT<WindowDrawCB> drawcalls = {nullptr, nullptr};

  /** Gesture stuff. */
  ListBaseT<wmGesture> gesture = {nullptr, nullptr};

  /**
   * Keep the last handled event in `event_queue` here (owned and must be freed).
   *
   * \warning This must only to be used for event queue logic.
   * User interactions should use `eventstate` instead (if the event isn't passed to the function).
   */
  wmEvent *event_last_handled = nullptr;

  /**
   * Storage for event system.
   *
   * For the most part this is storage for `wmEvent.xy` & `wmEvent.modifiers`.
   * newly added key/button events copy the cursor location and modifier state stored here.
   *
   * It's also convenient at times to be able to pass this as if it's a regular event.
   *
   * - This is not simply the current event being handled.
   *   The type and value is always set to the last press/release events
   *   otherwise cursor motion would always clear these values.
   *
   * - The value of `eventstate->modifiers` is set from the last pressed/released modifier key.
   *   This has the down side that the modifier value will be incorrect if users hold both
   *   left/right modifiers then release one. See note in #wm_event_add_ghostevent for details.
   */
  wmEvent *eventstate = nullptr;

  /**
   * The time when the key is pressed in milliseconds (see #GHOST_IEvent::getTime).
   * Used to detect double-click events.
   */
  uint64_t eventstate_prev_press_time_ms = 0;

  /** Private runtime info to show text in the status bar. */
  void *cursor_keymap_status = nullptr;

  /* Storage for staggered depsgraph evaluation. See `wm_staggered_eval_register`. */
  StaggeredEvalData staggered_eval;

  WindowRuntime() = default;
  ~WindowRuntime();
};

/**
 * Register an ID to be evaluated on full frames for the given range. Evaluation happens spread out
 * over time in the main event loop of Blender with a minimal depsgraph that covers all IDs that
 * should be evaluated. This avoids freezing Blender while the calculation runs.
 * Once complete, the ID is automatically deregistered from the evaluation list.
 *
 * If the given ID is already in the list of IDs to evaluate, the given range is combined with the
 * existing range for that ID.
 *
 * \param component_name is passed back into the callback and is up to the caller of this function
 * on how to use.
 * \param range determines the frames for which this ID shall be evaluated.
 * Inclusive at the start, exclusive at the end.
 * \param callback is the function that will be called for every frame in the given `range`.
 */
void wm_staggered_eval_register(WindowRuntime &runtime,
                                ID &id,
                                const StringRef component_name,
                                Bounds<int> range,
                                EvalCallback callback);

/**
 * Has to be called before calling `wm_staggered_eval_next_frame`. It is possible to do
 * consecutive calls to `wm_staggered_eval_next_frame` after calling prepare once.
 */
void wm_staggered_eval_prepare(Main &bmain, wmWindow &window);

/**
 * Runs the evaluation for the next frame and calls the callbacks of `StaggeredEvalTarget`.
 * The next frame is the closest frame to `current_frame` that is not inside `evaluated_range`.
 *
 * \returns true if the function can be called again to evaluate another frame. If false is
 * returned, `wm_staggered_eval_prepare` has to be called.
 */
bool wm_staggered_eval_next_frame(WindowRuntime &runtime, int current_frame);

}  // namespace bke
}  // namespace blender
