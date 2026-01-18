/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 *
 * Background image saving using BLI_task.
 * Works in both interactive and background render mode.
 */

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_string.h"
#include "BLI_task.h"

#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_image_format.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DNA_windowmanager_types.h"

#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "RE_pipeline.h"

#include "WM_api.hh"

#include "image_save_job.hh"

namespace blender::ed::space_image {

/* Track pending saves for queue limit. */
static std::atomic<int> g_pending_image_saves{0};

/* Flag to prevent new tasks during shutdown. */
static std::atomic<bool> g_image_save_shutting_down{false};

/* Global task pool for background image saves. */
static TaskPool *g_image_save_pool = nullptr;
static std::once_flag g_pool_init_flag;

/* Thread-safe report list for background save errors. */
static ReportList *g_image_save_reports = nullptr;

struct ImageSaveTaskData {
  ImBuf *ibuf;         /* Copied buffer (owned by task). */
  char filepath[1024]; /* FILE_MAX */
  ImageFormatData im_format;
  bool save_copy;
};

static void image_save_task_run(TaskPool *__restrict /*pool*/, void *taskdata)
{
  ImageSaveTaskData *task = static_cast<ImageSaveTaskData *>(taskdata);

  const bool success = BKE_imbuf_write_as(
      task->ibuf, task->filepath, &task->im_format, task->save_copy);

  if (success) {
    printf("Saved \"%s\"\n", task->filepath);
    fflush(stdout);
  }
  else {
    /* Thread-safe error reporting. */
    BKE_reportf(g_image_save_reports,
                RPT_ERROR,
                "Failed to save \"%s\": %s",
                task->filepath,
                strerror(errno));
    fprintf(stderr, "Failed to save \"%s\": %s\n", task->filepath, strerror(errno));
  }
}

static void image_save_task_free(TaskPool *__restrict /*pool*/, void *taskdata)
{
  ImageSaveTaskData *task = static_cast<ImageSaveTaskData *>(taskdata);

  if (task->ibuf) {
    IMB_freeImBuf(task->ibuf);
  }
  BKE_image_format_free(&task->im_format);
  MEM_freeN(task);

  /* Decrement pending count. */
  g_pending_image_saves.fetch_sub(1, std::memory_order_relaxed);
}

void image_save_pool_init()
{
  std::call_once(g_pool_init_flag, []() {
    /* Use 1/4 of available threads for background saves, minimum 1. */
    const int num_threads = max_ii(1, BLI_task_scheduler_num_threads() / 4);
    g_image_save_pool = BLI_task_pool_create_background_parallel(
        nullptr, TASK_PRIORITY_HIGH, num_threads);

    /* Create thread-safe report list for async errors. */
    g_image_save_reports = MEM_new<ReportList>(__func__);
    BKE_reports_init(g_image_save_reports, RPT_STORE);
  });
}

void image_save_pool_wait()
{
  if (g_image_save_pool != nullptr) {
    BLI_task_pool_work_and_wait(g_image_save_pool);
  }

  /* Transfer any accumulated reports to WindowManager. */
  if (g_image_save_reports != nullptr && !BLI_listbase_is_empty(&g_image_save_reports->list)) {
    wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
    if (wm) {
      WM_reports_from_reports_move(wm, g_image_save_reports);
    }
  }
}

void image_save_pool_exit()
{
  /* Prevent new tasks from being queued. */
  g_image_save_shutting_down.store(true, std::memory_order_release);

  if (g_image_save_pool != nullptr) {
    /* Wait for all pending saves to complete. */
    BLI_task_pool_work_and_wait(g_image_save_pool);
    BLI_task_pool_free(g_image_save_pool);
    g_image_save_pool = nullptr;
  }

  if (g_image_save_reports != nullptr) {
    BKE_reports_free(g_image_save_reports);
    MEM_delete(g_image_save_reports);
    g_image_save_reports = nullptr;
  }
}

bool image_save_background(bContext * /*C*/,
                           Image *ima,
                           ImageUser *iuser,
                           const ImageSaveOptions *opts)
{
  /* Reject new tasks during shutdown. */
  if (g_image_save_shutting_down.load(std::memory_order_acquire)) {
    return false;
  }

  /* Reject multilayer. */
  if (opts->im_format.imtype == R_IMF_IMTYPE_MULTILAYER) {
    return false;
  }

  /* Check queue limit (enforce minimum of 2 when enabled).
   * Use compare-exchange to atomically reserve a slot. */
  const int queue_limit = U.image_save_queue_limit;
  const int effective_limit = (queue_limit > 0) ? max_ii(2, queue_limit) : 0;
  if (effective_limit > 0) {
    int current = g_pending_image_saves.load(std::memory_order_relaxed);
    do {
      if (current >= effective_limit) {
        return false;
      }
    } while (!g_pending_image_saves.compare_exchange_weak(
        current, current + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
  }
  else {
    /* No limit - just increment. */
    g_pending_image_saves.fetch_add(1, std::memory_order_relaxed);
  }

  /* Ensure pool is initialized. */
  image_save_pool_init();
  if (g_image_save_pool == nullptr) {
    g_pending_image_saves.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  /* Acquire and copy buffer with color management applied. */
  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, iuser, &lock);
  if (!ibuf) {
    BKE_image_release_ibuf(ima, ibuf, lock);
    g_pending_image_saves.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  ImBuf *ibuf_cm = IMB_colormanagement_imbuf_for_write(
      ibuf, opts->save_as_render, true, &opts->im_format);

  ImBuf *ibuf_copy = (ibuf_cm == ibuf) ? IMB_dupImBuf(ibuf) : ibuf_cm;
  BKE_image_release_ibuf(ima, ibuf, lock);

  if (!ibuf_copy) {
    g_pending_image_saves.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  /* Create task data. */
  ImageSaveTaskData *task = static_cast<ImageSaveTaskData *>(
      MEM_callocN(sizeof(ImageSaveTaskData), __func__));
  if (!task) {
    IMB_freeImBuf(ibuf_copy);
    g_pending_image_saves.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }
  task->ibuf = ibuf_copy;
  STRNCPY(task->filepath, opts->filepath);
  task->im_format = opts->im_format;
  task->save_copy = opts->save_copy;

  /* Push task. */
  BLI_task_pool_push(
      g_image_save_pool, image_save_task_run, task, true, image_save_task_free);

  return true;
}

/* Task data for render result saves that need stamp metadata. */
struct RenderSaveTaskData {
  ImBuf *ibuf;         /* Copied buffer (owned by task). */
  char filepath[1024]; /* FILE_MAX */
  ImageFormatData im_format;
  RenderResult *stamp_rr; /* Render result for stamp data (copied). */
  bool stamp;
};

static void render_save_task_run(TaskPool *__restrict /*pool*/, void *taskdata)
{
  RenderSaveTaskData *task = static_cast<RenderSaveTaskData *>(taskdata);

  bool success;
  if (task->stamp && task->stamp_rr) {
    success = BKE_imbuf_write_stamp(
        nullptr, task->stamp_rr, task->ibuf, task->filepath, &task->im_format);
  }
  else {
    success = BKE_imbuf_write(task->ibuf, task->filepath, &task->im_format);
  }

  if (success) {
    printf("Saved \"%s\"\n", task->filepath);
    fflush(stdout);
  }
  else {
    /* Thread-safe error reporting. */
    BKE_reportf(g_image_save_reports,
                RPT_ERROR,
                "Failed to save \"%s\": %s",
                task->filepath,
                strerror(errno));
    fprintf(stderr, "Failed to save \"%s\": %s\n", task->filepath, strerror(errno));
  }
}

static void render_save_task_free(TaskPool *__restrict /*pool*/, void *taskdata)
{
  RenderSaveTaskData *task = static_cast<RenderSaveTaskData *>(taskdata);

  if (task->ibuf) {
    IMB_freeImBuf(task->ibuf);
  }
  if (task->stamp_rr) {
    RE_FreeRenderResult(task->stamp_rr);
  }
  BKE_image_format_free(&task->im_format);
  MEM_freeN(task);

  /* Decrement pending count. */
  g_pending_image_saves.fetch_sub(1, std::memory_order_relaxed);
}

bool image_save_background_render(ReportList * /*reports*/,
                                  RenderResult *rr,
                                  const Scene *scene,
                                  bool stamp,
                                  const char *filepath)
{
  /* Reject new tasks during shutdown. */
  if (g_image_save_shutting_down.load(std::memory_order_acquire)) {
    return false;
  }

  if (!rr) {
    return false;
  }

  const ImageFormatData *im_format = &scene->r.im_format;

  /* Reject multilayer EXR - too complex for background save. */
  if (im_format->imtype == R_IMF_IMTYPE_MULTILAYER) {
    return false;
  }

  /* Reject movies - not applicable. */
  if (BKE_imtype_is_movie(im_format->imtype)) {
    return false;
  }

  /* Reject multiview - complex cases need sync save. */
  if (RE_ResultIsMultiView(rr)) {
    return false;
  }

  /* Check queue limit (enforce minimum of 2 when enabled).
   * Use compare-exchange to atomically reserve a slot. */
  const int queue_limit = U.image_save_queue_limit;
  const int effective_limit = (queue_limit > 0) ? max_ii(2, queue_limit) : 0;
  if (effective_limit > 0) {
    int current = g_pending_image_saves.load(std::memory_order_relaxed);
    do {
      if (current >= effective_limit) {
        return false;
      }
    } while (!g_pending_image_saves.compare_exchange_weak(
        current, current + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
  }
  else {
    /* No limit - just increment. */
    g_pending_image_saves.fetch_add(1, std::memory_order_relaxed);
  }

  /* Ensure pool is initialized. */
  image_save_pool_init();
  if (g_image_save_pool == nullptr) {
    g_pending_image_saves.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  /* Create image format from scene settings. */
  ImageFormatData image_format;
  BKE_image_format_init_for_write(&image_format, scene, nullptr);

  const float dither = scene->r.dither_intensity;

  /* Get the ImBuf from the render result (view 0 for mono). */
  ImBuf *ibuf = RE_render_result_rect_to_ibuf(rr, &image_format, dither, 0);
  if (!ibuf) {
    BKE_image_format_free(&image_format);
    g_pending_image_saves.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  /* Apply color management. */
  IMB_colormanagement_imbuf_for_write(ibuf, true, false, &image_format);

  /* Create task data. */
  RenderSaveTaskData *task = static_cast<RenderSaveTaskData *>(
      MEM_callocN(sizeof(RenderSaveTaskData), __func__));
  if (!task) {
    IMB_freeImBuf(ibuf);
    BKE_image_format_free(&image_format);
    g_pending_image_saves.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }
  task->ibuf = ibuf;
  STRNCPY(task->filepath, filepath);
  task->im_format = image_format;
  task->stamp = stamp;

  /* Copy stamp data if needed. */
  if (stamp) {
    task->stamp_rr = RE_DuplicateRenderResult(rr);
  }
  else {
    task->stamp_rr = nullptr;
  }

  /* Push task. */
  BLI_task_pool_push(
      g_image_save_pool, render_save_task_run, task, true, render_save_task_free);

  return true;
}

}  // namespace blender::ed::space_image
