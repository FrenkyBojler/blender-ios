/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include <functional>

#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_vector.hh"

struct ThreadQueue;
struct TaskPool;

namespace blender::ed {

namespace detail {
class ResourceLoaderTaskPool;
struct ResourceLoaderJob;
}  // namespace detail

enum class ResourceLoadPriority {
  Low = 0,
  Medium = 1,
  High = 2,
};

template<typename CustomDataT> class AsyncResourceLoader {
  /** Executed on the worker thread. */
  using ResourceWorkFn = std::function<void(CustomDataT &)>;
  /** Executed on the worker thread. */
  using ProcessRequestFn = std::function<void(CustomDataT &)>;

  struct ResourceEntry {
    std::shared_ptr<CustomDataT> data;
    Vector<ProcessRequestFn> process_requests_fns;

    /** Count of current requests to this resource. Only if the last resource request is cancelled,
     * no further loading of the resource is done and the loader cancels. */
    std::atomic<int> ref_count = 0;
    std::atomic<ResourceLoadPriority> priority = ResourceLoadPriority::Low;
    std::atomic<bool> completed;
  };

  ResourceWorkFn resource_fn_;

  std::mutex requested_resources_mutex_;
  Map<std::string, std::shared_ptr<ResourceEntry>> requested_resources_;

  /* This doesn't need template parameters, so implemented as a separate class and inside the
   * source file. */
  std::unique_ptr<detail::ResourceLoaderTaskPool> task_pool_;

 public:
  /**
   * \param resource_fn: The function to load the resource. Executed once and on the worker thread.
   *     Typically this loads data and stores it in the passed custom data.
   */
  explicit AsyncResourceLoader(std::function<void(CustomDataT &)> resource_fn);

  /**
   * \param process_request_fn: Called after the resource was loaded. Each request can submit its
   *     own callback here, they will all be executed. Executed once and on the worker thread.
   */
  void request(std::string identifier,
               FunctionRef<CustomDataT()> create_custom_data,
               ProcessRequestFn process_request_fn = nullptr,
               ResourceLoadPriority priority = ResourceLoadPriority::Medium);

 private:
  /* Note that this can be called from the worker thread to re-queue a job. */
  void submit_job(StringRef identifier, ResourceLoadPriority priority);
  void work_single(const detail::ResourceLoaderJob &job);
};

namespace detail {

struct ResourceLoaderJob {
  std::string identifier;
  ResourceLoadPriority queued_priority;
};

class ResourceLoaderTaskPool {
 public:
  ResourceLoaderTaskPool() = default;
  ~ResourceLoaderTaskPool();

  ThreadQueue *job_queue_ = nullptr;
  TaskPool *task_pool_ = nullptr;

  void submit_job(StringRef identifier,
                  ResourceLoadPriority priority,
                  std::function<void(const ResourceLoaderJob &)> work_single_fn);
};

}  // namespace detail

template<typename CustomDataT>
inline AsyncResourceLoader<CustomDataT>::AsyncResourceLoader(ResourceWorkFn resource_fn)
    : resource_fn_(std::move(resource_fn)),
      task_pool_(std::make_unique<detail::ResourceLoaderTaskPool>())
{
}

template<typename CustomDataT>
inline void AsyncResourceLoader<CustomDataT>::request(
    std::string identifier,
    FunctionRef<CustomDataT()> create_custom_data,
    ProcessRequestFn process_request_fn,
    ResourceLoadPriority priority)
{

  std::unique_lock lock(requested_resources_mutex_);
  std::shared_ptr<ResourceEntry> &entry = requested_resources_.lookup_or_add_default_as(
      identifier);

  bool is_new = false;
  if (!entry) {
    entry = std::make_shared<ResourceEntry>();
    entry->data = std::make_shared<CustomDataT>(create_custom_data());
    entry->priority = priority;
    is_new = true;
  }

  /* If this is an existing request with lower priority than requested, increase priority. */
  ResourceLoadPriority old_priority = entry->priority.load();
  const bool priority_increased = int(priority) > int(old_priority);
  if (priority_increased) {
    entry->priority.store(priority);
  }

  entry->ref_count.fetch_add(1);
  if (process_request_fn) {
    entry->process_requests_fns.append(std::move(process_request_fn));
  }

  if (is_new ||
      /* If already queued/loading but priority increased, push a new job so worker will see it
       * earlier. */
      priority_increased)
  {
    this->submit_job(identifier, entry->priority.load());
  }
}

template<typename CustomDataT>
void AsyncResourceLoader<CustomDataT>::submit_job(const StringRef identifier,
                                                  ResourceLoadPriority priority)
{
  task_pool_->submit_job(identifier,
                         priority,
                         /* The job function executed on a thread. */
                         [this](const detail::ResourceLoaderJob &job) { work_single(job); });
}

/* Receives a job popped from the job queue. It is resilient to "stale" lower-priority job: when it
 * pops a Job, it checks the current request's priority and status; if the request's current
 * priority is higher than the popped job's priority, it re-queues a new job with that higher
 * priority and returns (so higher-priority jobs are processed first). */
template<typename CustomDataT>
void AsyncResourceLoader<CustomDataT>::work_single(const detail::ResourceLoaderJob &job)
{
  std::shared_ptr<CustomDataT> data = nullptr;

  {
    std::scoped_lock lock{requested_resources_mutex_};
    std::shared_ptr<ResourceEntry> *entry_ptr = requested_resources_.lookup_ptr_as(job.identifier);
    if (!entry_ptr) {
      return;
    }
    ResourceEntry *entry = entry_ptr->get();

    if (entry->completed) {
      return;
    }

    /* If priority of the request is higher than the priority the job was started with, requeue
     * higher priority job. This is quite cheap to do and easier than changing priority of a
     * running task. */
    ResourceLoadPriority current_priority = entry->priority.load();
    if (int(current_priority) > int(job.queued_priority)) {
      this->submit_job(job.identifier, current_priority);
      return;
    }

    data = entry->data;
  }

  resource_fn_(*data);

  {
    std::scoped_lock lock{requested_resources_mutex_};
    std::shared_ptr<ResourceEntry> *entry_ptr = requested_resources_.lookup_ptr_as(job.identifier);
    if (!entry_ptr) {
      return;
    }
    ResourceEntry *entry = entry_ptr->get();

    for (ProcessRequestFn &process_request_fn : entry->process_requests_fns) {
      process_request_fn(*entry->data);
    }

    if (entry->ref_count.load() == 0) {
      /* Cancelled request. */
      requested_resources_.remove(job.identifier);
      return;
    }
  }
}

}  // namespace blender::ed
