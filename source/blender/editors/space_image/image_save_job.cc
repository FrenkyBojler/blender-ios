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

#include "MEM_guardedalloc.h"

#include "BLI_string.h"
#include "BLI_task.h"

#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_image_format.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "RE_pipeline.h"

#include "image_save_job.hh"

namespace blender::ed::space_image {

/* Track pending saves for queue limit. */
static std::atomic<int> g_pending_image_saves{0};

/* Global task pool for background image saves. */
static TaskPool *g_image_save_pool = nullptr;

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
  }
  else {
    fprintf(stderr, "Failed to save \"%s\": %s\n", task->filepath, strerror(errno));
  }
}

static void image_save_task_free(TaskPool *__restrict /*pool*/, void *taskdata)
{
  ImageSaveTaskData *task = static_cast<ImageSaveTaskData *>(taskdata);

  if (task->ibuf) {
    IMB_freeImBuf(task->ibuf);
  }
  MEM_freeN(task);

  /* Decrement pending count. */
  g_pending_image_saves.fetch_sub(1);
}

void image_save_pool_init()
{
  if (g_image_save_pool == nullptr) {
    g_image_save_pool = BLI_task_pool_create_background(nullptr, TASK_PRIORITY_LOW);
  }
}

void image_save_pool_exit()
{
  if (g_image_save_pool != nullptr) {
    /* Wait for all pending saves to complete. */
    BLI_task_pool_work_and_wait(g_image_save_pool);
    BLI_task_pool_free(g_image_save_pool);
    g_image_save_pool = nullptr;
  }
}

bool image_save_background(bContext * /*C*/,
                           Image *ima,
                           ImageUser *iuser,
                           const ImageSaveOptions *opts)
{
  /* Reject multilayer. */
  if (opts->im_format.imtype == R_IMF_IMTYPE_MULTILAYER) {
    return false;
  }

  /* Check queue limit. */
  const int queue_limit = U.image_save_queue_limit;
  if (queue_limit > 0 && g_pending_image_saves.load() >= queue_limit) {
    return false;
  }

  /* Ensure pool is initialized. */
  image_save_pool_init();

  /* Acquire and copy buffer with color management applied. */
  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, iuser, &lock);
  if (!ibuf) {
    BKE_image_release_ibuf(ima, ibuf, lock);
    return false;
  }

  ImBuf *ibuf_cm = IMB_colormanagement_imbuf_for_write(
      ibuf, opts->save_as_render, true, &opts->im_format);

  ImBuf *ibuf_copy = (ibuf_cm == ibuf) ? IMB_dupImBuf(ibuf) : ibuf_cm;
  BKE_image_release_ibuf(ima, ibuf, lock);

  if (!ibuf_copy) {
    return false;
  }

  /* Create task data. */
  ImageSaveTaskData *task = static_cast<ImageSaveTaskData *>(
      MEM_callocN(sizeof(ImageSaveTaskData), __func__));
  task->ibuf = ibuf_copy;
  STRNCPY(task->filepath, opts->filepath);
  task->im_format = opts->im_format;
  task->save_copy = opts->save_copy;

  /* Increment pending count before queuing. */
  g_pending_image_saves.fetch_add(1);

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
  }
  else {
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
  g_pending_image_saves.fetch_sub(1);
}

bool image_save_background_render(ReportList * /*reports*/,
                                  RenderResult *rr,
                                  const Scene *scene,
                                  bool stamp,
                                  const char *filepath)
{
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

  /* Check queue limit. */
  const int queue_limit = U.image_save_queue_limit;
  if (queue_limit > 0 && g_pending_image_saves.load() >= queue_limit) {
    return false;
  }

  /* Ensure pool is initialized. */
  image_save_pool_init();

  /* Create image format from scene settings. */
  ImageFormatData image_format;
  BKE_image_format_init_for_write(&image_format, scene, nullptr);

  const float dither = scene->r.dither_intensity;

  /* Get the ImBuf from the render result (view 0 for mono). */
  ImBuf *ibuf = RE_render_result_rect_to_ibuf(rr, &image_format, dither, 0);
  if (!ibuf) {
    BKE_image_format_free(&image_format);
    return false;
  }

  /* Apply color management. */
  IMB_colormanagement_imbuf_for_write(ibuf, true, false, &image_format);

  /* Create task data. */
  RenderSaveTaskData *task = static_cast<RenderSaveTaskData *>(
      MEM_callocN(sizeof(RenderSaveTaskData), __func__));
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

  /* Increment pending count before queuing. */
  g_pending_image_saves.fetch_add(1);

  /* Push task. */
  BLI_task_pool_push(
      g_image_save_pool, render_save_task_run, task, true, render_save_task_free);

  return true;
}

}  // namespace blender::ed::space_image
