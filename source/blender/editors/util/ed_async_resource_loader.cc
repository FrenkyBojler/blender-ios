/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edutil
 */

#include <functional>

#include "BLI_task.h"
#include "BLI_threads.h"

#include "ED_async_resource_loader.hh"

namespace blender::ed::detail {

ResourceLoaderTaskPool::~ResourceLoaderTaskPool()
{
  if (job_queue_) {
    /* Any work left should have been cancelled at this point. */
    BLI_assert(BLI_thread_queue_is_empty(job_queue_));
    /* Signal background threads to stop waiting for new tasks if none are left. */
    BLI_thread_queue_nowait(job_queue_);
    /* But we still wait, in case the above assert fails. */
    BLI_thread_queue_wait_finish(job_queue_);
    BLI_thread_queue_free(job_queue_);
  }

  if (task_pool_) {
    BLI_task_pool_free(task_pool_);
  }
}

static constexpr ThreadQueueWorkPriority resource_to_thread_queue_priority(
    ResourceLoadPriority resource_priority)
{
  switch (resource_priority) {
    case ResourceLoadPriority::Low:
      return BLI_THREAD_QUEUE_WORK_PRIORITY_LOW;
    case ResourceLoadPriority::Medium:
      return BLI_THREAD_QUEUE_WORK_PRIORITY_NORMAL;
    case ResourceLoadPriority::High:
      return BLI_THREAD_QUEUE_WORK_PRIORITY_HIGH;
  }
}

void ResourceLoaderTaskPool::submit_job(
    const StringRef identifier,
    const ResourceLoadPriority priority,
    const std::function<void(const ResourceLoaderJob &)> work_single_fn)
{
  struct WrappedJob {
    ResourceLoaderJob job;
    std::function<void(const ResourceLoaderJob &)> fn;
  };

  WrappedJob *wrapped_job = MEM_new<WrappedJob>(__func__);
  wrapped_job->job.identifier = identifier;
  wrapped_job->job.queued_priority = priority;
  wrapped_job->fn = work_single_fn;

  // j.seq = seq_counter.fetch_add(1, std::memory_order_relaxed);
  if (!job_queue_) {
    job_queue_ = BLI_thread_queue_init();
  }
  BLI_thread_queue_push(job_queue_, wrapped_job, resource_to_thread_queue_priority(priority));

  if (!task_pool_) {
    task_pool_ = BLI_task_pool_create_background(job_queue_, TASK_PRIORITY_LOW);
  }
  BLI_task_pool_push(
      task_pool_,
      [](TaskPool *__restrict pool, void * /*taskdata*/) {
        ThreadQueue *job_queue = static_cast<ThreadQueue *>(BLI_task_pool_user_data(pool));

        WrappedJob *wrapped_job = static_cast<WrappedJob *>(
            BLI_thread_queue_pop_timeout(job_queue, 100));
        if (!wrapped_job) {
          return;
        }

        wrapped_job->fn(wrapped_job->job);

        MEM_delete(wrapped_job);
      },
      nullptr,
      false,
      nullptr);
}
}  // namespace blender::ed::detail
