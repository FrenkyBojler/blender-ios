/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_vector.hh"
#include "GPU_context.hh"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace blender::gpu {

enum class WorkPriority { Low, Medium, High };

using WorkCB = void (*)(void *);
using work_id = int64_t;

/* Abstracts the creation and management of secondary threads with GPU contexts.
 * Must be created from the main thread.
 * Threads and their context remain alive until destruction.
 */
class GPUWorker {
 private:
  Vector<std::unique_ptr<std::thread>> threads_;
  std::condition_variable condition_var_;
  std::mutex mutex_;
  bool terminate_ = false;

  std::unique_ptr<class WorkQueue> work_queue_;

 public:
  enum class ContextType {
    /** Use the main GPU context on the worker threads. */
    Main,
    /** Use a different secondary GPU context for each worker thread. */
    PerThread,
  };

  GPUWorker(uint32_t threads_count, ContextType context_type);
  ~GPUWorker();

  work_id push_work(WorkCB callback, void *payload, WorkPriority priority);
  void remove_work(work_id id);
  bool is_empty();

 private:
  void run(std::shared_ptr<GPUSecondaryContext> context);
};

}  // namespace blender::gpu
