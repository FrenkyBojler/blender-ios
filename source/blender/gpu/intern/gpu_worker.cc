/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GPU_worker.hh"

namespace blender::gpu {

GPUWorker::GPUWorker(uint32_t threads_count,
                     ContextType context_type,
                     std::function<void()> run_cb)
{
  for (int i : IndexRange(threads_count)) {
    UNUSED_VARS(i);
    std::shared_ptr<GPUSecondaryContext> thread_context =
        context_type == ContextType::PerThread ? std::make_shared<GPUSecondaryContext>() : nullptr;
    threads_.append(std::make_unique<std::thread>([=]() { this->run(thread_context, run_cb); }));
  }
}

GPUWorker::~GPUWorker()
{
  {
    std::unique_lock<std::mutex> lock(mutex_);
    terminate_ = true;
  }
  condition_var_.notify_all();
  for (std::unique_ptr<std::thread> &thread : threads_) {
    thread->join();
  }
}

void GPUWorker::run(std::shared_ptr<GPUSecondaryContext> context, std::function<void()> run_cb)
{
  if (context) {
    context->activate();
  }

  /* Loop until we get the terminate signal. */
  while (true) {
    {
      /* Wait until we have work to do, or until termination. */
      std::unique_lock<std::mutex> lock(mutex_);
      condition_var_.wait(lock, [&]() { return pending_works_ || terminate_; });
      if (terminate_) {
        break;
      }
      pending_works_--;
    }

    run_cb();
  }
}

}  // namespace blender::gpu
