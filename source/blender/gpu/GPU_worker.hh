/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_vector.hh"
#include "GPU_context.hh"

#include "GHOST_C-api.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace blender::gpu {

class GPUSecondaryContext {
 private:
  GHOST_ContextHandle ghost_context_;
  GPUContext *gpu_context_;

 public:
  GPUSecondaryContext();
  ~GPUSecondaryContext();

  /* Must be called from a secondary thread.*/
  void activate();
};

class GPUWorker {
 private:
  Vector<std::unique_ptr<std::thread>> threads_;
  std::condition_variable condition_var_;
  std::mutex mutex_;
  std::atomic_bool terminate_ = false;

 public:
  enum class ContextType {
    Main,
    Shared,
    PerThread,
  };

  GPUWorker(uint32_t threads_count, ContextType context_type, std::function<void()> run_cb);
  ~GPUWorker();

  void wake_up()
  {
    condition_var_.notify_one();
  }

 private:
  void run(std::shared_ptr<GPUSecondaryContext> context, std::function<void()> run_cb);
};

}  // namespace blender::gpu
