/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_threads.h"
#include "BLI_vector.hh"
#include "GPU_context.hh"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace blender::gpu {

using WorkCB = void (*)(void *);
using work_id = uint64_t;

/* Abstracts the creation and management of secondary threads with GPU contexts.
 * Must be created from the main thread.
 * Threads and their context remain alive until destruction.
 */
class GPUWorker {
 private:
  Vector<std::unique_ptr<std::thread>> threads_;
  ThreadQueue *work_queue_;
  WorkCB callback;

 public:
  enum class ContextType {
    /** Use the main GPU context on the worker threads. */
    Main,
    /** Use a different secondary GPU context for each worker thread. */
    PerThread,
  };

  GPUWorker(uint32_t threads_count, ContextType context_type, WorkCB callback);
  ~GPUWorker();

  work_id push_work(void *work, ThreadQueueWorkPriority priority);
  void cancel_work(work_id id);
  bool is_empty();

 private:
  void run(std::shared_ptr<GPUSecondaryContext> context);
};

}  // namespace blender::gpu
