#include "BLI_bounds.hh"
#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "WM_types.hh"
#include "WM_api.hh"

#include "ED_anim_api.hh"

namespace blender::animviz {

struct TargetData {
  EvaluationTarget target;
  void *buffer;
  EvalCallback eval;
  UpdateCallback update;
  FinishCallback finish;
  bool finished_left = false;
  bool finished_right = false;

  TargetData(const EvaluationTarget &target,
             void *buffer,
             EvalCallback eval,
             UpdateCallback update,
             FinishCallback finish)
      : target(target), buffer(buffer), eval(eval), update(update), finish(finish)
  {
  }
};

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
  Vector<TargetData> target_data;
};

struct BGEvalJobData {
  /* Has to stay first. */
  WorkerData worker_data;
  /* All targets currently evaluating. */
  Set<EvaluationTarget> active_targets;
};

static void run_job(void *job_data, wmJobWorkerStatus *worker_status)
{
  WorkerData *eval_data = static_cast<WorkerData *>(job_data);
  eval_data->restart.store(false, std::memory_order_release);
  const int center_frame = eval_data->evaluation_center.load(std::memory_order_acquire);
  eval_data->evaluated_range = {center_frame, center_frame + 1};

  while (true) {
    int frame;
    /* TODO what about frame skipping i.e. every second frame. Would be determined by the
     * evaluation targets. */
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
    bool all_done = true;
    DEG_evaluate_on_framechange(eval_data->dg, frame);
    for (TargetData &target_data : eval_data->target_data) {
      const EvaluationTarget &target = target_data.target;
      void *target_buffer = target_data.buffer;
      const bool modified_data = target_data.eval(eval_data->dg, *target.id, frame, target_buffer);
      if (!modified_data) {
        if (frame < center_frame) {
          target_data.finished_left = true;
        }
        else {
          target_data.finished_right = true;
        }
      }
      all_done &= target_data.finished_left && target_data.finished_right;
    }
    worker_status->do_update = true;

    if (all_done) {
      break;
    }
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
                              const EvaluationTarget &target,
                              void *target_buffer,
                              EvalCallback eval_cb,
                              UpdateCallback update_cb,
                              FinishCallback finish_cb)
{
  wmJob *wm_job = WM_jobs_get(
      &wm, &window, &scene, job_name, eWM_JobFlag(0), WM_JOB_TYPE_MOTION_PATH_EVAL);

  if (WM_jobs_is_running(wm_job)) {
    BGEvalJobData *eval_data = static_cast<BGEvalJobData *>(WM_jobs_customdata_get(wm_job));
    const bool id_already_registered = eval_data->active_targets.contains(target);
    if (id_already_registered) {
      eval_data->worker_data.restart.store(true, std::memory_order_release);
      return;
    }
    /* If the job is running and the ID is not yet registered, we have to kill it so we can modify
     * BGEvalJobData without race conditions. This is a blocking call. */
    WM_jobs_kill_type(&wm, &scene, WM_JOB_TYPE_MOTION_PATH_EVAL);
  }

  BGEvalJobData *eval_data = MEM_new<BGEvalJobData>(__func__);
  eval_data->active_targets.add(target);
  eval_data->worker_data.target_data.append(
      {target, target_buffer, eval_cb, update_cb, finish_cb});

  Depsgraph *dg = DEG_graph_new(&bmain, &scene, &view_layer, DAG_EVAL_VIEWPORT);
  /* TODO merge ID list with existing IDs. */
  DEG_graph_build_from_ids(dg, {target.id});
  /* Evaluate once on the main thread so the copy on eval nodes have run. */
  DEG_evaluate_on_refresh(dg);
  /* Don't allow reading main from the worker thread. */
  DEG_set_allow_read_from_main(dg, false);
  eval_data->worker_data.dg = dg;

  WM_jobs_customdata_set(wm_job, eval_data, free_job_data);
  WM_jobs_callbacks(wm_job, run_job, nullptr, update_job, finish_job);
  WM_jobs_start(&wm, wm_job);
}

void background_eval_deregister(wmWindowManager &wm,
                                wmWindow &window,
                                Scene &scene,
                                const EvaluationTarget &target)
{
  wmJob *wm_job = WM_jobs_get(
      &wm, &window, &scene, job_name, eWM_JobFlag(0), WM_JOB_TYPE_MOTION_PATH_EVAL);

  if (!WM_jobs_is_running(wm_job)) {
    /* No job to deregister from. */
    return;
  }

  /* TODO */
}

}  // namespace animviz
