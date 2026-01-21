/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup render
 *
 * Unified background image save system.
 *
 * This module provides asynchronous image saving for both viewport and final rendering.
 * The design is lifted from the battle-tested viewport rendering code (render_opengl.cc)
 * and generalized for reuse.
 *
 * Key design principles:
 * - Self-contained tasks: All scene-dependent data is extracted BEFORE queueing
 * - Thread safety: Deep copies of RenderResult, no scene pointer in tasks
 * - Memory management: Automatic heuristics to decide sync vs async
 */

#include "background_save.hh"

#include "MEM_guardedalloc.h"

#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_system.h"
#include "BLI_task.h"
#include "BLI_task.hh"

#include "DNA_scene_types.h"

#include "BKE_image.hh"
#include "BKE_image_format.hh"
#include "BKE_image_save.hh"

#include "RE_background_save.h"
#include "RE_pipeline.h"

#include "CLG_log.h"

namespace blender::render {

static CLG_LogRef LOG = {"render.background_save"};

/* -------------------------------------------------------------------- */
/** \name Global State
 * \{ */

static BackgroundSaveState g_state;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Self-Contained Task Data
 *
 * All data needed for background saving, with NO scene pointer.
 * All scene-dependent data is extracted at queue time.
 * \{ */

/**
 * Self-contained task data for background image save.
 * NO scene pointer - all data extracted at queue time.
 * Uses RAII for cleanup.
 */
struct BackgroundSaveTask {
  /** Deep copy of render result (owned). */
  RenderResult *rr = nullptr;

  /** Image format settings (owned, deep copied). */
  ImageFormatData im_format = {};

  /** Output file path. */
  char filepath[FILE_MAX] = "";

  /** Optional preview file path (for EXR with R_IMF_FLAG_PREVIEW_JPG). */
  char preview_filepath[FILE_MAX] = "";

  /** Frame number (for logging/debugging). */
  int frame = 0;

  /** Whether to apply stamp metadata. */
  bool use_stamp = false;

  /** Dither intensity from scene. */
  float dither = 0.0f;

  ~BackgroundSaveTask()
  {
    if (rr) {
      RE_FreeRenderResult(rr);
    }
    BKE_image_format_free(&im_format);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pool Management
 * \{ */

/**
 * Ensure pool is initialized. Must be called while holding g_state.mutex.
 * \return true if pool is available, false on failure.
 */
static bool ensure_pool_initialized_locked()
{
  if (g_state.initialized) {
    return g_state.image_pool != nullptr;
  }

  /* Image pool: parallel execution OK. */
  g_state.image_pool = BLI_task_pool_create(nullptr, TASK_PRIORITY_HIGH);
  if (!g_state.image_pool) {
    CLOG_ERROR(&LOG, "Failed to create background save image pool");
    g_state.initialized = true;
    return false;
  }

  g_state.initialized = true;
  return true;
}

/**
 * Try to reserve a slot in the queue.
 * \return true if slot reserved, false if queue is full.
 */
static bool try_reserve_slot()
{
  std::scoped_lock lock(g_state.mutex);
  if (g_state.pending_count >= MAX_SCHEDULED_FRAMES) {
    return false;
  }
  g_state.pending_count++;
  return true;
}

/** Release a slot in the queue. */
static void release_slot()
{
  std::scoped_lock lock(g_state.mutex);
  BLI_assert(g_state.pending_count > 0);
  g_state.pending_count--;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Task Execution
 * \{ */

/**
 * Execute the background save task. Thread-safe, no scene access.
 */
static void background_save_task_execute(BackgroundSaveTask *task)
{
  BLI_assert(task != nullptr);
  BLI_assert(task->rr != nullptr);

  const char *preview_path = task->preview_filepath[0] ? task->preview_filepath : nullptr;
  bool success = BKE_image_render_write_from_rr(
      task->rr, &task->im_format, task->filepath, task->dither, task->use_stamp, 0, preview_path);

  if (success) {
    CLOG_INFO(&LOG, "Saved frame %d: \"%s\"", task->frame, task->filepath);
  }
  else {
    std::scoped_lock lock(g_state.mutex);
    g_state.failed_count++;
  }
}

/** Task pool callback for running the save task. */
static void background_save_task_run(TaskPool *__restrict /*pool*/, void *taskdata)
{
  BackgroundSaveTask *task = static_cast<BackgroundSaveTask *>(taskdata);

  /* Isolate task to prevent nested parallelism from causing issues. */
  threading::isolate_task([task]() { background_save_task_execute(task); });
}

/** Task pool callback for freeing the task data. */
static void background_save_task_free(TaskPool *__restrict /*pool*/, void *taskdata)
{
  BackgroundSaveTask *task = static_cast<BackgroundSaveTask *>(taskdata);
  MEM_delete(task);
  release_slot();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public C API
 * \{ */

void background_save_init_impl()
{
  std::scoped_lock lock(g_state.mutex);
  g_state.shutting_down = false;
  ensure_pool_initialized_locked();
}

void background_save_exit_impl()
{
  TaskPool *image_pool = nullptr;

  /* Get pool pointer and mark as shutting down under lock. */
  {
    std::scoped_lock lock(g_state.mutex);
    g_state.shutting_down = true;
    image_pool = g_state.image_pool;
  }

  /* Wait for pending saves WITHOUT holding the lock (tasks need it for release_slot). */
  if (image_pool) {
    BLI_task_pool_work_and_wait(image_pool);
  }

  /* Now free pool and reset state under lock. */
  {
    std::scoped_lock lock(g_state.mutex);
    if (g_state.image_pool) {
      BLI_task_pool_free(g_state.image_pool);
      g_state.image_pool = nullptr;
    }
    g_state.pending_count = 0;
    g_state.failed_count = 0;
    g_state.initialized = false;
    g_state.shutting_down = false;
  }
}

void background_save_wait_impl()
{
  TaskPool *image_pool = nullptr;

  /* Get pool pointer under lock. */
  {
    std::scoped_lock lock(g_state.mutex);
    image_pool = g_state.image_pool;
  }

  /* Wait WITHOUT holding the lock (tasks need it for release_slot). */
  if (image_pool) {
    BLI_task_pool_work_and_wait(image_pool);
  }
}

bool background_save_render_impl(RenderResult *rr,
                                 const Scene *scene,
                                 const Object *camera,
                                 const char *filepath)
{
  if (!rr || !scene || !filepath) {
    return false;
  }

  const ImageFormatData *scene_format = &scene->r.im_format;

  /* Reject movie formats - not supported for background save. */
  if (BKE_imtype_is_movie(scene_format->imtype)) {
    return false;
  }

  /* Reject multilayer EXR - too complex for background save. */
  if (scene_format->imtype == R_IMF_IMTYPE_MULTILAYER) {
    return false;
  }

  /* Reject multiview - complex cases need sync save. */
  if (RE_ResultIsMultiView(rr)) {
    return false;
  }

  /* Check if we can reserve a slot (backpressure). */
  if (!try_reserve_slot()) {
    CLOG_DEBUG(&LOG, "Queue full, falling back to sync save for frame %d", scene->r.cfra);
    return false;
  }

  /* Create task data. All scene-dependent data is extracted here. */
  BackgroundSaveTask *task = MEM_new<BackgroundSaveTask>(__func__);

  /* Deep copy the render result. */
  task->rr = RE_DuplicateRenderResult(rr);
  if (!task->rr) {
    MEM_delete(task);
    release_slot();
    CLOG_ERROR(&LOG, "Failed to duplicate render result for frame %d", scene->r.cfra);
    return false;
  }

  /* Always populate/refresh stamp data to capture final render stats. */
  if (scene->r.stamp & R_STAMP_ALL) {
    BKE_render_result_stamp_info(const_cast<Scene *>(scene),
                                 const_cast<Object *>(camera),
                                 task->rr,
                                 false);
  }

  /* Copy image format settings. */
  BKE_image_format_init_for_write(&task->im_format, scene, nullptr);

  /* Copy other scene-dependent data. */
  STRNCPY(task->filepath, filepath);
  task->frame = scene->r.cfra;
  task->use_stamp = (scene->r.stamp & R_STAMP_ALL) != 0;
  task->dither = scene->r.dither_intensity;

  /* Compute preview filepath for EXR with preview flag. */
  const bool is_exr = ELEM(
      task->im_format.imtype, R_IMF_IMTYPE_OPENEXR, R_IMF_IMTYPE_MULTILAYER);
  if (is_exr && (task->im_format.flag & R_IMF_FLAG_PREVIEW_JPG)) {
    STRNCPY(task->preview_filepath, filepath);
    /* Replace .exr extension with .jpg. */
    if (BLI_path_extension_check(task->preview_filepath, ".exr")) {
      task->preview_filepath[strlen(task->preview_filepath) - 4] = '\0';
    }
    BKE_image_path_ext_from_imtype_ensure(
        task->preview_filepath, sizeof(task->preview_filepath), R_IMF_IMTYPE_JPEG90);
  }

  /* Queue the task. */
  {
    std::scoped_lock lock(g_state.mutex);

    if (g_state.shutting_down) {
      MEM_delete(task);
      release_slot();
      return false;
    }

    if (!ensure_pool_initialized_locked()) {
      MEM_delete(task);
      release_slot();
      return false;
    }

    BLI_task_pool_push(
        g_state.image_pool, background_save_task_run, task, true, background_save_task_free);
  }

  CLOG_DEBUG(&LOG, "Queued frame %d for background save", task->frame);
  return true;
}

bool background_save_should_use_impl(size_t peak_memory_mb)
{
  /* Get system memory info (no lock needed for system calls). */
  size_t total_mb = BLI_system_memory_max_in_megabytes();
  size_t used_mb = MEM_get_memory_in_use() / (1024 * 1024);
  size_t available_mb = (total_mb > used_mb) ? (total_mb - used_mb) : 0;

  /* Heuristic 1: If render used > 50% of total, copying would risk OOM. */
  if (peak_memory_mb > total_mb / 2) {
    CLOG_DEBUG(&LOG,
               "Sync save: peak memory (%zuMB) > 50%% of total (%zuMB)",
               peak_memory_mb,
               total_mb);
    return false;
  }

  /* Heuristic 2: If less than 500MB available, be conservative. */
  constexpr size_t MIN_AVAILABLE_MB = 500;
  if (available_mb < MIN_AVAILABLE_MB) {
    CLOG_DEBUG(
        &LOG, "Sync save: available memory (%zuMB) < %zuMB", available_mb, MIN_AVAILABLE_MB);
    return false;
  }

  /* Lock for accessing shared state. */
  std::scoped_lock lock(g_state.mutex);

  /* Heuristic 3: If queue is heavily loaded, fall back to sync. */
  int pending = g_state.pending_count;
  if (pending >= MAX_SCHEDULED_FRAMES) {
    CLOG_DEBUG(&LOG, "Sync save: queue full (%d pending)", pending);
    return false;
  }

  return true;
}

int background_save_get_failed_count_impl()
{
  std::scoped_lock lock(g_state.mutex);
  return g_state.failed_count;
}

void background_save_clear_failed_count_impl()
{
  std::scoped_lock lock(g_state.mutex);
  g_state.failed_count = 0;
}

}  // namespace blender::render

/** \} */

/* API functions - defined outside namespace to match header. */

void RE_background_save_init()
{
  blender::render::background_save_init_impl();
}

void RE_background_save_exit()
{
  blender::render::background_save_exit_impl();
}

void RE_background_save_wait()
{
  blender::render::background_save_wait_impl();
}

bool RE_background_save_render(blender::RenderResult *rr,
                               const blender::Scene *scene,
                               const blender::Object *camera,
                               const char *filepath)
{
  return blender::render::background_save_render_impl(rr, scene, camera, filepath);
}

bool RE_background_save_should_use(size_t peak_memory_mb)
{
  return blender::render::background_save_should_use_impl(peak_memory_mb);
}

int RE_background_save_get_failed_count()
{
  return blender::render::background_save_get_failed_count_impl();
}

void RE_background_save_clear_failed_count()
{
  blender::render::background_save_clear_failed_count_impl();
}
