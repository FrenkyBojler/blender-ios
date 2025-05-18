/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <chrono>
#include <condition_variable>
#include <fmt/format.h>
#include <mutex>
#include <thread>

#include "BKE_appdir.hh"
#include "BKE_global.hh"
#include "BKE_screen.hh"
#include "BKE_undo_system.hh"
#include "BLF_api.hh"
#include "BLI_listbase.h"
#include "BLI_mutex.hh"
#include "BLI_rect.h"
#include "BLI_stack.hh"
#include "BLI_threads.h"
#include "BLI_vector_set.hh"
#include "BLO_writefile.hh"
#include "BLT_translation.hh"
#include "BPY_extern_run.hh"
#include "GHOST_C-api.h"
#include "GPU_context.hh"
#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "GPU_viewport.hh"
#include "UI_interface.hh"
#include "WM_api.hh"
#include "WM_blocking_work.hh"

#include "ED_screen.hh"

#include "DNA_screen_types.h"

#include "GPU_framebuffer.hh"
#include "wm_draw.hh"
#include "wm_window.hh"
#include "wm_window_private.hh"

namespace blender::blocking_work {

/* Use system clock because that's also used internally generally.
 * https://en.cppreference.com/w/cpp/thread/condition_variable/wait_until */
using Clock = std::chrono::system_clock;

struct DoneInfo {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
};

struct StatusInfo {
  std::string message;
  Clock::time_point start_time;
};

static Vector<StatusInfo> get_current_status_messages();

class BlockingWorkHandler {
 private:
  bContext &C_;
  wmWindow &window_;
  DoneInfo &done_info_;
  Clock::time_point start_time_;
  int2 cursor_{};
  bool show_dialog_ = false;
  rcti status_bar_rect_{};
  Vector<StatusInfo> status_infos_;

 public:
  BlockingWorkHandler(bContext &C,
                      wmWindow &window,
                      DoneInfo &done,
                      const Clock::time_point start_time)
      : C_(C), window_(window), done_info_(done), start_time_(start_time)
  {
  }

  void on_wait_time_expired()
  {
    GHOST_SystemHandle g_system = wm_ghost_system_handle_get();

    wm_skip_events_on_main_loop = true;
    BLI_SCOPED_DEFER([&]() { wm_skip_events_on_main_loop = false; });

    /* Dispatch all events received so far, so that they can be ignored by the dialog. */
    if (GHOST_ProcessEvents(g_system, false)) {
      GHOST_DispatchEvents(g_system);
    }

    wmWindowManager *wm = CTX_wm_manager(&C_);
    /* Ignore all events received until the dialog opened. */
    const wmEvent *last_handled_event = static_cast<const wmEvent *>(
        window_.runtime->event_queue.last);

    while (true) {
      {
        std::lock_guard lock{done_info_.mutex};
        if (done_info_.done) {
          /* The task finished, no need to recover anymore. */
          return;
        }
      }

      auto handle_event = [&](const wmEvent &event) {
        if (event.type == EVT_RETKEY && event.val == KM_PRESS) {
          /* Actually try to recover the file. This function terminates the current process. */
          this->try_recover_file(C_);
        }
        if (ISMOUSE(event.type)) {
          cursor_ = event.xy;
        }
        if (event.type == LEFTMOUSE && event.val == KM_PRESS) {
          if (BLI_rcti_isect_pt_v(&status_bar_rect_, event.xy)) {
            show_dialog_ = !show_dialog_;
          }
        }
      };

      if (GHOST_ProcessEvents(g_system, false)) {
        GHOST_DispatchEvents(g_system);
        const wmEvent *first_event = last_handled_event ? last_handled_event->next :
                                                          static_cast<wmEvent *>(
                                                              window_.runtime->event_queue.first);
        for (const wmEvent *event = first_event; event; event = event->next) {
          handle_event(*event);
        }
        last_handled_event = static_cast<wmEvent *>(window_.runtime->event_queue.last);
      }

      status_infos_ = get_current_status_messages();

      this->draw_window_with_dialog(*wm, window_);

      /* Sleep to avoid keeping the thread busy all the time, which takes up resources that could
       * be used by the actual computation. */
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
  }

  void draw_window_background()
  {
    /* Clearing looks better when the window is resized while the cancel dialog is open. */
    GPU_clear_color(0, 0, 0, 1);

    /* Draw the last Blender screen. */
    bScreen *screen = WM_window_get_active_screen(&window_);
    ED_screen_areas_iter (&window_, screen, area) {
      LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
        if (!region->runtime->visible) {
          continue;
        }
        if (region->overlap) {
          continue;
        }
        if (!region->runtime->draw_buffer) {
          continue;
        }
        if (region->runtime->draw_buffer->viewport) {
          GPU_viewport_draw_to_screen(region->runtime->draw_buffer->viewport, 0, &region->winrct);
        }
        else {
          GPU_offscreen_draw_to_screen(
              region->runtime->draw_buffer->offscreen, region->winrct.xmin, region->winrct.ymin);
        }
      }
    }
  }

  void draw_dialog()
  {
    const Clock::time_point current_time = Clock::now();
    const int seconds_since_start =
        std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time_).count();

    uiFontStyle fstyle = *UI_FSTYLE_WIDGET;

    const std::string message = fmt::format(
        IFACE_("Looks like it takes a while to finish this computation ({}s). You can just keep "
               "waiting or try to recover the session by pressing enter."),
        seconds_since_start);

    UI_fontstyle_set(&fstyle);
    float message_width, message_height;
    BLF_width_and_height(
        fstyle.uifont_id, message.c_str(), message.size(), &message_width, &message_height);

    const int dialog_width = window_.sizex / 2;
    const int dialog_height = window_.sizey / 2;
    rctf dialog_rect{};
    dialog_rect.xmin = (window_.sizex - dialog_width) / 2;
    dialog_rect.xmax = dialog_rect.xmin + dialog_width;
    dialog_rect.ymin = (window_.sizey - dialog_height) / 2;
    dialog_rect.ymax = dialog_rect.ymin + dialog_height;
    UI_draw_roundbox_4fv(&dialog_rect, true, 10, float4(0.2, 0.2, 0.2, 1.0));

    uchar font_color[4] = {255, 255, 255, 255};
    UI_fontstyle_draw_simple(&fstyle,
                             (window_.sizex - message_width) / 2,
                             (window_.sizey - message_height) / 2,
                             message.c_str(),
                             font_color);
  }

  void draw_status()
  {
    const SpaceType *stype = BKE_spacetype_from_id(SPACE_STATUSBAR);
    const ARegionType *art = BKE_regiontype_from_id(stype, RGN_TYPE_HEADER);
    const int status_bar_height = art->prefsizey;
    const int progress_ring_padding = 2;
    const int progress_ring_radius_outer = status_bar_height / 2 - progress_ring_padding;
    const int progress_ring_radius_inner = progress_ring_radius_outer - 3;

    const uiStyle &style = *UI_style_get();
    const uiFontStyle &fs = style.widget;

    const StringRefNull status_message = this->get_status_bar_message();
    const int status_message_width = BLF_width(
        fs.uifont_id, status_message.c_str(), status_message.size());
    const int status_message_padding = UI_UNIT_X * 0.2f;

    const float duration_s =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_time_).count() /
        1000.0f;
    const int start_x = window_.sizex / 2;
    const int total_width = 2.0f * progress_ring_radius_outer + status_message_padding +
                            status_message_width;
    const int outer_padding = UI_UNIT_X * 0.3f;

    rcti bg_rect{};
    bg_rect.xmin = start_x - outer_padding;
    bg_rect.xmax = start_x + total_width + outer_padding;
    bg_rect.ymin = 0;
    bg_rect.ymax = status_bar_height;

    status_bar_rect_ = bg_rect;

    bTheme &theme = *UI_GetTheme();
    const ColorTheme4b status_bar_bg_color = UI_ThemeGetColorPtr(
        &theme, SPACE_STATUSBAR, TH_HEADER);

    rctf rectf;
    BLI_rctf_rcti_copy(&rectf, &bg_rect);
    UI_draw_roundbox_4fv(&rectf, true, 0, status_bar_bg_color.to_4f());

    const bool is_hovered = BLI_rcti_isect_pt_v(&bg_rect, cursor_);
    if (is_hovered) {
      ColorTheme4f hover_color;
      UI_GetThemeColorShade4fv(TH_HEADER, 10, hover_color);
      UI_draw_roundbox_4fv(&rectf, true, 2, hover_color);
    }

    const ColorTheme4b text_color = UI_ThemeGetColorPtr(&theme, SPACE_STATUSBAR, TH_HEADER_TEXT);

    int current_x = start_x;

    GPUVertFormat *format = immVertexFormat();
    const uint format_pos = GPU_vertformat_attr_add(
        format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor4ubv(text_color);
    GPU_blend(GPU_BLEND_ALPHA);

    const float ring_end = fmod(duration_s, 1.0f);
    const float ring_start = std::max(ring_end - (1 - ring_end), 0.0f);

    imm_draw_disk_partial_fill_2d(format_pos,
                                  current_x + progress_ring_radius_outer,
                                  progress_ring_padding + progress_ring_radius_outer,
                                  progress_ring_radius_inner,
                                  progress_ring_radius_outer,
                                  48,
                                  ring_start * 360.0f,
                                  (ring_end - ring_start) * 360.0f);

    immUnbindProgram();

    current_x += progress_ring_radius_outer * 2.0f;
    current_x += status_message_padding;

    uiFontStyleDraw_Params params{};
    params.align = UI_STYLE_TEXT_LEFT;
    rcti status_message_rect{};
    status_message_rect.xmin = current_x;
    status_message_rect.xmax = current_x + status_message_width;
    status_message_rect.ymin = 0;
    status_message_rect.ymax = status_bar_height;
    UI_fontstyle_draw(
        &fs, &status_message_rect, status_message.c_str(), UI_MAX_DRAW_STR, text_color, &params);
  }

  void draw_window_with_dialog(wmWindowManager &wm, wmWindow &window)
  {
    GPU_context_main_lock();
    BLI_SCOPED_DEFER([&]() { GPU_context_main_unlock(); });

    GPU_render_begin();

    wm_window_make_drawable(&wm, &window);
    wmWindowViewport(&window);
    {
      GPUContext *gpu_context = static_cast<GPUContext *>(window.gpuctx);
      GPU_context_begin_frame(gpu_context);
      GPU_bgl_end();
      this->draw_window_background();
      this->draw_status();
      if (show_dialog_) {
        this->draw_dialog();
      }
      GPU_context_end_frame(gpu_context);
    }
    wm_window_swap_buffers(&window);

    GPU_render_end();
  }

  [[noreturn]] void try_recover_file(bContext &C)
  {
    wmWindowManager *wm = CTX_wm_manager(&C);

    /* Undo one step. The idea here is that the last change "broke" the file. E.g. this could
     * happen by accidentally setting the subdivision levels too high. This last change needs to be
     * undone, before saving the file. */
    BKE_undosys_step_undo(wm->undo_stack, &C);

    /* Determine file path for the recovery file. */
    Main *bmain = CTX_data_main(&C);
    const char *tempdir_base = BKE_tempdir_base();
    char filepath[FILE_MAX];
    BLI_path_join(filepath, FILE_MAX, tempdir_base, "cancel_recover.blend");

    /* Save the recovery file. */
    BlendFileWriteParams blend_write_params{};
    BLO_write_file(bmain, filepath, G_FILE_RECOVER_WRITE, &blend_write_params, nullptr);

    /* Open new Blender session with the saved file. Uses Python because C++ does not have good
     * standard libraries for this. */
    const char *imports[] = {"subprocess", "bpy", nullptr};
    const std::string expr = fmt::format(
        "subprocess.Popen([bpy.app.binary_path, r\"{}\"], start_new_session=True)", filepath);
    BPY_run_string_exec(nullptr, imports, expr.c_str());

    /* Attempt to close window as soon as possible as this feels better. The OS may take a bit
     * longer to clean up all the resources of the process. */
    LISTBASE_FOREACH (wmWindow *, window_iter, &wm->windows) {
      wm_ghostwindow_destroy(wm, window_iter);
    }

    /* Terminate this process because it's in an invalid state now and may use up many resources.
     */
    std::terminate();
  }

  StringRefNull get_status_bar_message() const
  {
    if (status_infos_.is_empty()) {
      return IFACE_("Compute");
    }
    const StatusInfo *status_info = std::max_element(
        status_infos_.begin(), status_infos_.end(), [](const StatusInfo &a, const StatusInfo &b) {
          return a.start_time < b.start_time;
        });
    return status_info->message;
  }
};

static bContext *g_context = nullptr;

void set_global_context(bContext &C)
{
  g_context = &C;
}

static wmWindow *pick_window_for_dialog(wmWindowManager &wm)
{
  LISTBASE_FOREACH (wmWindow *, window, &wm.windows) {
    GHOST_TWindowState state = GHOST_GetWindowState(
        static_cast<GHOST_WindowHandle>(window->ghostwin));
    if (state == GHOST_kWindowStateMinimized) {
      continue;
    }
    return window;
  }
  return nullptr;
}

static bool g_exit_cancel_worker_thread = false;

void run(const FunctionRef<void()> fn)
{
  if (g_exit_cancel_worker_thread) {
    fn();
    return;
  }
  if (!BLI_thread_is_main()) {
    fn();
    return;
  }
  if (!g_context) {
    fn();
    return;
  }
  bContext &C = *g_context;
  wmWindowManager *wm = CTX_wm_manager(&C);
  if (!wm) {
    fn();
    return;
  }
  // const bool can_undo = wm->undo_stack && wm->undo_stack->step_active &&
  //                       wm->undo_stack->step_active->prev;
  // if (!can_undo) {
  //   fn();
  //   return;
  // }
  wmWindow *window = pick_window_for_dialog(*wm);
  if (!window) {
    fn();
    return;
  }

  const std::chrono::milliseconds wait_time{3000};
  const Clock::time_point start_time = Clock::now();
  const Clock::time_point wait_expired_time = start_time +
                                              std::chrono::duration_cast<Clock::duration>(
                                                  wait_time);

  struct WorkerThreadTask {
    std::mutex mutex;
    std::condition_variable cv;
    const FunctionRef<void()> *fn = nullptr;
    DoneInfo *done_info = nullptr;
  };

  static WorkerThreadTask task;

  /* Only use a single worker thread instead of creating a new one for every invocation. */
  static std::thread worker_thread{[]() {
    while (true) {
      /* Wait until there is a task. */
      {
        std::unique_lock lock{task.mutex};
        task.cv.wait(lock, [&]() { return task.fn; });
      }

      /* Actually run task. */
      (*task.fn)();

      /* Clear the task. */
      {
        std::unique_lock lock{task.mutex};
        task.fn = nullptr;
      }

      /* Notify the main thread that the task is done. */
      {
        std::unique_lock lock{task.done_info->mutex};
        task.done_info->done = true;
      }
      task.done_info->cv.notify_one();

      if (g_exit_cancel_worker_thread) {
        return;
      }
    }
  }};
  BLI_SCOPED_DEFER([&]() {
    if (g_exit_cancel_worker_thread) {
      worker_thread.join();
    }
  });

  /* Schedule the task on the worker thread. */
  DoneInfo done;
  {
    std::lock_guard lock{task.mutex};
    BLI_assert(task.fn == nullptr);
    task.fn = &fn;
    task.done_info = &done;
  }
  task.cv.notify_one();

  /* Wait until done or wait time is up. */
  {
    std::unique_lock lock{done.mutex};
    if (done.cv.wait_until(lock, wait_expired_time, [&]() { return done.done; })) {
      return;
    }
  }
  BlockingWorkHandler handler(C, *window, done, start_time);
  /* This call may never return if recovery is attempted. */
  handler.on_wait_time_expired();
}

void exit_worker_thread()
{
  run([]() { g_exit_cancel_worker_thread = true; });
}

struct LocalStatusStack;

struct StatusStacks {
  Mutex mutex;
  RawVectorSet<LocalStatusStack *> stacks;
};

static std::shared_ptr<StatusStacks> get_status_stacks()
{
  static std::shared_ptr<StatusStacks> status_stacks = std::make_shared<StatusStacks>();
  return status_stacks;
}

struct StatusScopeStorage {
  StatusScope *scope = nullptr;
  StatusInfo info;
};

struct LocalStatusStack : NonCopyable, NonMovable {
 private:
  std::shared_ptr<StatusStacks> owner_;

 public:
  mutable Mutex mutex;
  Stack<StatusScopeStorage> stack;

  LocalStatusStack()
  {
    owner_ = get_status_stacks();
    std::lock_guard lock{owner_->mutex};
    owner_->stacks.add(this);
  }

  ~LocalStatusStack()
  {
    std::lock_guard lock{owner_->mutex};
    owner_->stacks.remove(this);
  }
};

static LocalStatusStack &get_local_status_stack()
{
  static thread_local LocalStatusStack local_status_stack;
  return local_status_stack;
}

StatusScope::StatusScope(std::string message)
{
  LocalStatusStack &stack = get_local_status_stack();
  std::lock_guard lock{stack.mutex};
  stack.stack.push({this, {std::move(message), Clock::now()}});
}

StatusScope::~StatusScope()
{
  LocalStatusStack &stack = get_local_status_stack();
  std::lock_guard lock{stack.mutex};
  StatusScopeStorage popped = stack.stack.pop();
  BLI_assert(popped.scope == this);
}

static Vector<StatusInfo> get_current_status_messages()
{
  Vector<StatusInfo> infos;
  std::shared_ptr<StatusStacks> status_stacks = get_status_stacks();
  std::lock_guard stacks_lock{status_stacks->mutex};
  for (const LocalStatusStack *stack : status_stacks->stacks) {
    std::lock_guard stack_lock{stack->mutex};
    if (!stack->stack.is_empty()) {
      infos.append(stack->stack.peek().info);
    }
  }
  return infos;
}

}  // namespace blender::blocking_work

bool wm_skip_events_on_main_loop = false;
