/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GPU_worker.hh"

#include "deque"

namespace blender::gpu {

struct Work {
  WorkCB callback = nullptr;
  void *payload = nullptr;
  work_id id = 0;
};

class WorkQueue {
 private:
  static inline work_id current_id = 0;

  std::deque<Work> low_priority_;
  std::deque<Work> normal_priority_;
  std::deque<Work> high_priority_;

 public:
  work_id push(WorkCB callback, void *payload, WorkPriority priority)
  {
    Work work = {callback, payload, ++current_id};

    switch (priority) {
      case WorkPriority::Low:
        low_priority_.push_back(work);
        break;
      case WorkPriority::Medium:
        normal_priority_.push_back(work);
        break;
      case WorkPriority::High:
        high_priority_.push_back(work);
        break;
      default:
        BLI_assert_unreachable();
        break;
    }

    return current_id;
  }

  Work pop()
  {
    if (!high_priority_.empty()) {
      Work work = high_priority_.front();
      high_priority_.pop_front();
      return work;
    }
    if (!normal_priority_.empty()) {
      Work work = normal_priority_.front();
      normal_priority_.pop_front();
      return work;
    }
    if (!low_priority_.empty()) {
      Work work = low_priority_.front();
      low_priority_.pop_front();
      return work;
    }
    return {};
  }

  bool is_empty()
  {
    return low_priority_.empty() && normal_priority_.empty() && high_priority_.empty();
  }

  void remove_work(work_id id)
  {
    auto remove = [id](std::deque<Work> &queue) {
      queue.erase(std::remove_if(
                      queue.begin(), queue.end(), [&](const Work elem) { return elem.id == id; }),
                  queue.end());
    };

    remove(low_priority_);
    remove(normal_priority_);
    remove(high_priority_);
  }
};

GPUWorker::GPUWorker(uint32_t threads_count, ContextType context_type)
{
  work_queue_ = std::make_unique<WorkQueue>();

  for (int i : IndexRange(threads_count)) {
    UNUSED_VARS(i);
    std::shared_ptr<GPUSecondaryContext> thread_context =
        context_type == ContextType::PerThread ? std::make_shared<GPUSecondaryContext>() : nullptr;
    threads_.append(std::make_unique<std::thread>([=]() { this->run(thread_context); }));
  }
}

GPUWorker::~GPUWorker()
{
  BLI_assert(work_queue_->is_empty());
  {
    std::unique_lock<std::mutex> lock(mutex_);
    terminate_ = true;
  }
  condition_var_.notify_all();
  for (std::unique_ptr<std::thread> &thread : threads_) {
    thread->join();
  }
}

work_id GPUWorker::push_work(WorkCB callback, void *payload, WorkPriority priority)
{
  work_id id = 0;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    id = work_queue_->push(callback, payload, priority);
  }
  condition_var_.notify_one();
  return id;
}

void GPUWorker::remove_work(work_id id)
{
  std::unique_lock<std::mutex> lock(mutex_);
  work_queue_->remove_work(id);
}

bool GPUWorker::is_empty()
{
  return work_queue_->is_empty();
}

void GPUWorker::run(std::shared_ptr<GPUSecondaryContext> context)
{
  if (context) {
    context->activate();
  }

  /* Loop until we get the terminate signal. */
  while (true) {
    Work work = {};
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_var_.wait(lock, [&]() {
        work = work_queue_->pop();
        return work.id || terminate_;
      });
      if (terminate_) {
        break;
      }
    }
    work.callback(work.payload);
  }
}

}  // namespace blender::gpu
