/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup render
 *
 * Internal header for background image save system.
 */

#pragma once

#include <deque>
#include <mutex>

#include "BLI_sys_types.h"
#include "BLI_task.h"

namespace blender {
struct Object;
struct RenderResult;
struct Scene;
}  // namespace blender

namespace blender::render {

/**
 * Maximum number of frames that can be queued for background saving.
 * Provides backpressure to prevent memory exhaustion.
 * Matches the limit from viewport rendering (render_opengl.cc).
 */
constexpr int MAX_SCHEDULED_FRAMES = 8;

/**
 * Global state for the background save system.
 * Modeled after the OGLRender pattern from viewport rendering.
 */
struct BackgroundSaveState {
  std::mutex mutex;

  /** Pool for image saves (parallel execution OK). */
  blender::TaskPool *image_pool = nullptr;

  /** Pending task count (for backpressure). Protected by mutex. */
  int pending_count = 0;

  /** Failed save count (for error reporting). Protected by mutex. */
  int failed_count = 0;

  /** Whether the system has been initialized. */
  bool initialized = false;

  /** Whether shutdown is in progress (prevents new tasks). */
  bool shutting_down = false;

  /** Queue of completed saves awaiting callback. Protected by mutex. */
  std::deque<int> completed_frames;
};

/* Internal implementation functions (called by C API wrappers). */
void background_save_init_impl();
void background_save_exit_impl();
void background_save_wait_impl();
bool background_save_render_impl(RenderResult *rr,
                                 const Scene *scene,
                                 const Object *camera,
                                 const char *filepath);
bool background_save_should_use_impl(size_t peak_memory_mb);
int background_save_get_failed_count_impl();
void background_save_clear_failed_count_impl();
int background_save_drain_completed_impl(int *out_frames, int max_frames);
void background_save_push_completed_impl(int frame);
bool background_save_has_pending_impl();

}  // namespace blender::render
