/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */
#include "MEM_guardedalloc.h"

#include "DNA_action_types.h"
#include "DNA_object_types.h"

#include "BLT_translation.hh"

#include "BKE_anim_visualization.h"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "WM_api.hh"

#include "GPU_batch.hh"

#include "BLO_read_write.hh"

namespace blender {

/* ******************************************************************** */
/* Animation Visualization */

void animviz_settings_init(bAnimVizSettings *avs)
{
  /* sanity check */
  if (avs == nullptr) {
    return;
  }

  /* path settings */
  avs->path_bc = avs->path_ac = 10;

  avs->path_sf = 1;   /* XXX: Take from scene instead? */
  avs->path_ef = 250; /* XXX: Take from scene instead? */

  avs->path_viewflag = MOTIONPATH_VIEW_KFRAS | MOTIONPATH_VIEW_KFNOS;

  avs->path_step = 1;

  avs->path_bakeflag |= MOTIONPATH_BAKE_HEADS;
}

/* ------------------- */

void animviz_free_motionpath_cache(bMotionPath *mpath)
{
  /* sanity check */
  if (mpath == nullptr) {
    return;
  }

  /* free the path if necessary */
  if (mpath->points) {
    MEM_delete(mpath->points);
  }

  GPU_VERTBUF_DISCARD_SAFE(mpath->points_vbo);
  GPU_BATCH_DISCARD_SAFE(mpath->batch_line);
  GPU_BATCH_DISCARD_SAFE(mpath->batch_points);

  /* reset the relevant parameters */
  mpath->points = nullptr;
  mpath->length = 0;
}

void animviz_free_motionpath(bMotionPath *mpath)
{
  /* sanity check */
  if (mpath == nullptr) {
    return;
  }

  /* free the cache first */
  animviz_free_motionpath_cache(mpath);

  /* now the instance itself */
  MEM_delete(mpath);
}

/* ------------------- */

bMotionPath *animviz_copy_motionpath(const bMotionPath *mpath_src)
{
  bMotionPath *mpath_dst;

  if (mpath_src == nullptr) {
    return nullptr;
  }

  mpath_dst = MEM_dupalloc(mpath_src);
  mpath_dst->points = MEM_dupalloc(mpath_src->points);

  /* should get recreated on draw... */
  mpath_dst->points_vbo = nullptr;
  mpath_dst->batch_line = nullptr;
  mpath_dst->batch_points = nullptr;

  return mpath_dst;
}

/* ------------------- */

bMotionPath *animviz_verify_motionpaths(ReportList *reports,
                                        Scene *scene,
                                        Object *ob,
                                        bPoseChannel *pchan)
{
  bAnimVizSettings *avs;
  bMotionPath *mpath, **dst;

  /* sanity checks */
  if (ELEM(nullptr, scene, ob)) {
    return nullptr;
  }

  /* get destination data */
  if (pchan) {
    /* Paths for pose-channel - assume that pose-channel belongs to the object. */
    avs = &ob->pose->avs;
    dst = &pchan->mpath;
  }
  else {
    /* paths for object */
    avs = &ob->avs;
    dst = &ob->mpath;
  }

  /* Avoid 0 size allocations. */
  if (avs->path_sf >= avs->path_ef) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Motion path frame extents invalid for %s (%d to %d)%s",
                (pchan) ? pchan->name : ob->id.name,
                avs->path_sf,
                avs->path_ef,
                (avs->path_sf == avs->path_ef) ? RPT_(", cannot have single-frame paths") : "");
    return nullptr;
  }

  /* Adding 1 because the avs range is inclusive on both ends. */
  const int expected_length = (avs->path_ef - avs->path_sf) + 1;
  BLI_assert(expected_length > 1); /* Because the `if` above. */

  /* If there is already a motionpath, just return that, provided its settings
   * are ok (saves extra free+alloc). */
  if (*dst != nullptr) {
    mpath = *dst;

    if (avs->path_bakeflag & MOTIONPATH_BAKE_CAMERA_SPACE) {
      mpath->flag |= MOTIONPATH_FLAG_BAKE_CAMERA;
    }
    else {
      mpath->flag &= ~MOTIONPATH_FLAG_BAKE_CAMERA;
    }

    /* Only reuse a path if it was already a valid path, and of the expected length. */
    if (mpath->start_frame != mpath->end_frame && mpath->length == expected_length) {
      mpath->start_frame = avs->path_sf;
      mpath->end_frame = avs->path_ef + 1;
      return mpath;
    }

    /* Clear the existing cache, to allocate a new one below. */
    animviz_free_motionpath_cache(mpath);
  }
  else {
    mpath = MEM_new<bMotionPath>("bMotionPath");
    *dst = mpath;
  }

  /* Copy mpath settings from the viz settings. */
  mpath->start_frame = avs->path_sf;
  mpath->end_frame = avs->path_ef + 1;
  mpath->length = expected_length;

  if (avs->path_bakeflag & MOTIONPATH_BAKE_HEADS) {
    mpath->flag |= MOTIONPATH_FLAG_BHEAD;
  }
  else {
    mpath->flag &= ~MOTIONPATH_FLAG_BHEAD;
  }

  if (avs->path_bakeflag & MOTIONPATH_BAKE_CAMERA_SPACE) {
    mpath->flag |= MOTIONPATH_FLAG_BAKE_CAMERA;
  }
  else {
    mpath->flag &= ~MOTIONPATH_FLAG_BAKE_CAMERA;
  }

  /* Set default custom values (RGB). */
  mpath->color[0] = 1.0;
  mpath->color[1] = 0.0;
  mpath->color[2] = 0.0;

  mpath->color_post[0] = 0.1;
  mpath->color_post[1] = 1.0;
  mpath->color_post[2] = 0.1;

  mpath->line_thickness = 2;
  mpath->flag |= MOTIONPATH_FLAG_LINES;

  /* Allocate a cache. */
  mpath->points = MEM_new_array<bMotionPathVert>(mpath->length, "bMotionPathVerts");

  /* Tag viz settings as currently having some path(s) which use it. */
  avs->path_bakeflag |= MOTIONPATH_BAKE_HAS_PATHS;

  return mpath;
}

void animviz_motionpath_blend_write(BlendWriter *writer, bMotionPath *mpath)
{
  /* sanity checks */
  if (mpath == nullptr) {
    return;
  }

  /* firstly, just write the motionpath struct */
  writer->write_struct(mpath);

  /* now write the array of data */
  writer->write_struct_array(mpath->length, mpath->points);
}

void animviz_motionpath_blend_read_data(BlendDataReader *reader, bMotionPath *mpath)
{
  /* sanity check */
  if (mpath == nullptr) {
    return;
  }

  /* relink points cache */
  BLO_read_array_and_validate_size(reader, &mpath->points, &mpath->length);

  mpath->points_vbo = nullptr;
  mpath->batch_line = nullptr;
  mpath->batch_points = nullptr;
}

namespace animviz {

/* Data visible to the worker thread. */
struct WorkerData {
  /* Can be set from the main thread to tell the evaluating thread to start over.
   * Is used because we cannot just stop the depsgraph evaluation. */
  std::atomic<bool> restart;
  /* The center frame around which to run the evaluation. */
  std::atomic<int> evaluation_center;
  /* The range of frames that were already evaluated. Inclusive/Exclusive. */
  Bounds<int> evaluated_range;

  /* The depsgraph to evaluate in the background. All copy on eval nodes have to be
   * evaluated before it is sent to the thread. */
  Depsgraph *dg = nullptr;
};

struct BGEvalJobData {
  WorkerData worker_data;

  /* The IDs that are being evaluated. */
  Array<ID *> ids;
};

static void run_job(void *job_data, wmJobWorkerStatus *worker_status)
{
  WorkerData *eval_data = static_cast<WorkerData *>(job_data);
  eval_data->restart.store(false, std::memory_order_release);
  const int center_frame = eval_data->evaluation_center.load(std::memory_order_acquire);
  eval_data->evaluated_range = {center_frame, center_frame + 1};

  while (true) {
    int frame;
    if (abs(eval_data->evaluated_range.min - center_frame) <
        abs(eval_data->evaluated_range.max - center_frame))
    {
      frame = eval_data->evaluated_range.min - 1;
      eval_data->evaluated_range.min -= 1;
    }
    else {
      frame = eval_data->evaluated_range.max;
      eval_data->evaluated_range.max += 1;
    }

    DEG_evaluate_on_framechange(eval_data->dg, frame);
  }
}

static void update_job(void *job_data)
{
  WorkerData *eval_data = static_cast<WorkerData *>(job_data);
}

static void finish_job(void *job_data)
{
  WorkerData *eval_data = static_cast<WorkerData *>(job_data);
}

static void free_job_data(void *job_data)
{
  BGEvalJobData *eval_data = static_cast<BGEvalJobData *>(job_data);
  DEG_graph_free(eval_data->worker_data.dg);
  MEM_delete(eval_data);
}

constexpr const char *job_name = "Async Evaluate Frame Range";

void background_eval_register(Main &bmain,
                              wmWindowManager &wm,
                              wmWindow &window,
                              Scene &scene,
                              ViewLayer &view_layer,
                              ID &id,
                              EvalCallback buffer_cb,
                              UpdateCallback update_cb)
{
  wmJob *wm_job = WM_jobs_get(
      &wm, &window, &scene, job_name, eWM_JobFlag(0), WM_JOB_TYPE_MOTION_PATH_EVAL);

  if (WM_jobs_is_running(wm_job)) {
    BGEvalJobData *eval_data = static_cast<BGEvalJobData *>(WM_jobs_customdata_get(wm_job));
    const bool id_already_registered = eval_data->ids.as_span().contains(&id);
    if (id_already_registered) {
      eval_data->worker_data.restart.store(true, std::memory_order_release);
      return;
    }
    /* If the job is running and the ID is not yet registered, we have to kill it so we can modify
     * BGEvalJobData without race conditions. This is a blocking call. */
    WM_jobs_kill_type(&wm, &scene, WM_JOB_TYPE_MOTION_PATH_EVAL);
  }

  BGEvalJobData *eval_data = MEM_new<BGEvalJobData>(__func__);
  Depsgraph *dg = DEG_graph_new(&bmain, &scene, &view_layer, DAG_EVAL_VIEWPORT);
  /* TODO merge ID list with existing IDs. */
  DEG_graph_build_from_ids(dg, {&id});
  /* Evaluate once on the main thread so the copy on eval nodes have run. */
  DEG_evaluate_on_refresh(dg);
  /* Don't allow reading main from the worker thread. */
  DEG_set_allow_read_from_main(dg, false);
  eval_data->worker_data.dg = dg;

  WM_jobs_customdata_set(wm_job, eval_data, free_job_data);
  WM_jobs_callbacks(wm_job, run_job, nullptr, update_job, finish_job);
  WM_jobs_start(&wm, wm_job);
}

void background_eval_deregister(wmWindowManager &wm, wmWindow &window, Scene &scene)
{
  wmJob *wm_job = WM_jobs_get(
      &wm, &window, &scene, job_name, eWM_JobFlag(0), WM_JOB_TYPE_MOTION_PATH_EVAL);

  if (!WM_jobs_is_running(wm_job)) {
    /* No job to deregister from. */
    return;
  }
}

void background_eval_set_center_frame(const int frame)
{
  /* wmJob *wm_job = WM_jobs_get(
      &wm, &window, &scene, job_name, eWM_JobFlag(0), WM_JOB_TYPE_MOTION_PATH_EVAL); */
}

}  // namespace animviz

}  // namespace blender
