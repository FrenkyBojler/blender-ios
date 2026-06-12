/* SPDX-FileCopyrightText: 2016 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "BKE_global.hh"

#include "GPU_capabilities.hh"
#include "GPU_context.hh"
#include "GPU_state.hh"

#include "RE_engine.h"
#include "RE_pipeline.h"

#include "DRW_engine.hh"
#include "DRW_render.hh"

#include "WM_api.hh"
#include "wm_window.hh"

namespace blender {

using namespace blender::gpu;

/* -------------------------------------------------------------------- */
/** \name Submission critical section
 *
 * The usage of gpu::Shader objects is currently not thread safe. Since they are shared resources
 * between render engine instances, we cannot allow pass submissions in a concurrent manner.
 * \{ */

static TicketMutex *draw_mutex = nullptr;
static TicketMutex *submission_mutex = nullptr;

void DRW_mutexes_init()
{
  draw_mutex = BLI_ticket_mutex_alloc();
  submission_mutex = BLI_ticket_mutex_alloc();
}

void DRW_mutexes_exit()
{
  BLI_ticket_mutex_free(draw_mutex);
  BLI_ticket_mutex_free(submission_mutex);
}

void DRW_lock_start()
{
  bool locked = BLI_ticket_mutex_lock_check_recursive(draw_mutex);
  BLI_assert(locked);
  UNUSED_VARS_NDEBUG(locked);
}

void DRW_lock_end()
{
  BLI_ticket_mutex_unlock(draw_mutex);
}

void DRW_submission_start()
{
  bool locked = BLI_ticket_mutex_lock_check_recursive(submission_mutex);
  BLI_assert(locked);
  UNUSED_VARS_NDEBUG(locked);
  GPU_render_begin();
}

void DRW_submission_end()
{
  GPU_render_end();
  BLI_ticket_mutex_unlock(submission_mutex);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ContextShared
 *
 * \{ */

/* Context that can be shared across threads. Usage is guarded by a ticket mutex.
 * Should eventually be moved to GPU module after we get rid of the WM calls. */
class ContextShared {
  /* Should be private but needs to be public for XR workaround. */
 public:
  TicketMutex *mutex_ = nullptr;
  GPUSecondaryContextData context_;

  /* NOTE: This changes the active context. */
  ContextShared()
  {
    mutex_ = BLI_ticket_mutex_alloc();

    context_ = GPU_create_secondary_context();
  }

  ~ContextShared()
  {
    GPU_deactivate_secondary_context(context_);
    GPU_destroy_secondary_context(context_);

    BLI_ticket_mutex_free(mutex_);
  }

  void enable()
  {
    DRW_lock_start();
    /* IMPORTANT: We don't support immediate mode in render mode!
     * This shall remain in effect until immediate mode supports
     * multiple threads. */
    BLI_ticket_mutex_lock(mutex_);

    GPU_render_begin();

    GPU_activate_secondary_context(context_);
    GPU_context_begin_frame(context_.gpu_context);
  }

  bool is_enabled()
  {
    return context_.gpu_context == GPU_context_active_get();
  }

  /* Restore window drawable after disabling if restore is true. */
  void disable(bool restore = false)
  {
    GPU_context_end_frame(context_.gpu_context);

    if (BLI_thread_is_main() && restore) {
      wm_window_reset_drawable();
    }
    else {
      GPU_deactivate_secondary_context(context_);
    }
    /* Render boundaries are opened and closed here as this may be
     * called outside of an existing render loop. */
    GPU_render_end();

    BLI_ticket_mutex_unlock(mutex_);
    DRW_lock_end();
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU & System Context
 *
 * A global GPUContext is used for rendering every viewports (even on different windows).
 * This is because some resources cannot be shared between contexts (GPUFramebuffers, GPUBatch).
 * \{ */

/** Unique context used by Viewports. */
static ContextShared *viewport_context = nullptr;
/** Unique context used by Preview jobs. */
static ContextShared *preview_context = nullptr;

void DRW_gpu_context_create()
{
  BLI_assert(viewport_context == nullptr); /* Ensure it's called once */

  DRW_mutexes_init();

  viewport_context = MEM_new<ContextShared>(__func__);
  preview_context = MEM_new<ContextShared>(__func__);

  viewport_context->enable();
}

void DRW_gpu_context_destroy()
{
  BLI_assert(BLI_thread_is_main());
  if (viewport_context == nullptr) {
    return;
  }
  DRW_mutexes_exit();

  MEM_SAFE_DELETE(viewport_context);
  MEM_SAFE_DELETE(preview_context);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw GPU Context
 * \{ */

void DRW_gpu_context_enable_ex(bool /*restore*/)
{
  if (viewport_context == nullptr) {
    return;
  }
  viewport_context->enable();
}

void DRW_gpu_context_disable_ex(bool restore)
{
  if (viewport_context == nullptr) {
    return;
  }
  viewport_context->disable(restore);
}

static void drw_gpu_preview_context_enable()
{
  if (preview_context == nullptr) {
    return;
  }
  preview_context->enable();
}

static void drw_gpu_preview_context_disable()
{
  if (preview_context == nullptr) {
    return;
  }
  preview_context->disable();
}

void DRW_gpu_context_enable()
{
  /* TODO: should be replace by a more elegant alternative. */

  if (G.background && viewport_context == nullptr) {
    WM_init_gpu();
  }
  DRW_gpu_context_enable_ex(true);
}

bool DRW_gpu_context_try_enable()
{
  if (viewport_context == nullptr) {
    return false;
  }
  DRW_gpu_context_enable_ex(true);
  return true;
}

bool DRW_gpu_context_is_enabled()
{
  return viewport_context && viewport_context->is_enabled();
}

void DRW_gpu_context_disable()
{
  DRW_gpu_context_disable_ex(true);
}

void DRW_system_gpu_render_context_enable(const GPUSecondaryContextData &re_system_gpu_context)
{
  /* If thread is main you should use DRW_gpu_context_enable(). */
  BLI_assert(!BLI_thread_is_main());

  DRW_lock_start();
  GPU_activate_secondary_context(re_system_gpu_context);
}

void DRW_system_gpu_render_context_disable(const GPUSecondaryContextData &re_system_gpu_context)
{
  GPU_flush();
  GPU_deactivate_secondary_context(re_system_gpu_context);
  DRW_lock_end();
}

void DRW_render_context_enable(Render *render)
{
  if (G.background && viewport_context == nullptr) {
    WM_init_gpu();
  }

  GPU_render_begin();

  if (GPU_use_main_context_workaround()) {
    GPU_context_main_lock();
    DRW_gpu_context_enable();
    return;
  }

  GPUSecondaryContextData gpu_context = RE_system_gpu_context_get(render);

  /* Changing Context */
  if (gpu_context.is_initialized()) {
    DRW_system_gpu_render_context_enable(gpu_context);
  }
  else {
    drw_gpu_preview_context_enable();
  }
}

void DRW_render_context_disable(Render *render)
{
  if (GPU_use_main_context_workaround()) {
    DRW_gpu_context_disable();
    GPU_render_end();  // TODO: check
    GPU_context_main_unlock();
    return;
  }

  GPUSecondaryContextData gpu_context = RE_system_gpu_context_get(render);

  if (gpu_context.is_initialized()) {
    GPU_render_end();
    DRW_system_gpu_render_context_disable(gpu_context);
  }
  else {
    GPU_render_end();  // TODO:check
    /* Usually the case for a preview job. The `Render` is created inside the render thread which
     * is too late to create a GPU context. */
    drw_gpu_preview_context_disable();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name XR
 * \{ */

#ifdef WITH_XR_OPENXR

GPUSecondaryContextData DRW_gpu_context_get()
{
  /* XXX: There should really be no such getter, but for VR we currently can't easily avoid it.
   * OpenXR needs some low level info for the GPU context that will be used for submitting the
   * final frame-buffer. VR could in theory create its own context, but that would mean we have to
   * switch to it just to submit the final frame, which has notable performance impact.
   *
   * We could "inject" a context through DRW_system_gpu_render_context_enable(), but that would
   * have to work from the main thread, which is tricky to get working too. The preferable solution
   * would be using a separate thread for VR drawing where a single context can stay active. */

  return viewport_context->context_;
}

void DRW_xr_drawing_begin()
{
  /* XXX: See comment on #DRW_gpu_context_get(). */

  DRW_lock_start();
  BLI_ticket_mutex_lock(viewport_context->mutex_);
}

void DRW_xr_drawing_end()
{
  /* XXX: See comment on #DRW_gpu_context_get(). */

  BLI_ticket_mutex_unlock(viewport_context->mutex_);
  DRW_lock_end();
}

#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw manager context release/activation
 *
 * These functions are used in cases when an GPU context creation is needed during the draw.
 * This happens, for example, when an external engine needs to create its own GPU context from
 * the engine initialization.
 *
 * Example of context creation:
 *
 *   const bool drw_state = DRW_gpu_context_release();
 *   viewport_system_gpu_context = GPU_create_secondary_context();
 *   DRW_gpu_context_activate(drw_state);
 *
 * Example of context destruction:
 *
 *   const bool drw_state = DRW_gpu_context_release();
 *   GPU_activate_secondary_context(viewport_system_gpu_context);
 *   GPU_destroy_secondary_context(viewport_system_gpu_context);
 *   DRW_gpu_context_activate(drw_state);
 *
 *
 * NOTE: Will only perform context modification when on main thread. This way these functions can
 * be used in an engine without check on whether it is a draw manager which manages GPU context
 * on the current thread. The downside of this is that if the engine performs GPU creation from
 * a non-main thread, that thread is supposed to not have GPU context ever bound by Blender.
 *
 * \{ */

bool DRW_gpu_context_release()
{
  if (!BLI_thread_is_main()) {
    return false;
  }

  if (GPU_context_active_get() != viewport_context->context_.gpu_context) {
    /* Context release is requested from the outside of the draw manager main draw loop, indicate
     * this to the `DRW_gpu_context_activate()` so that it restores drawable of the window.
     */
    return false;
  }

  GPU_deactivate_secondary_context(viewport_context->context_);

  return true;
}

void DRW_gpu_context_activate(bool drw_state)
{
  if (!BLI_thread_is_main()) {
    return;
  }

  if (drw_state) {
    GPU_activate_secondary_context(viewport_context->context_);
  }
  else {
    wm_window_reset_drawable();
  }
}

/** \} */

}  // namespace blender
