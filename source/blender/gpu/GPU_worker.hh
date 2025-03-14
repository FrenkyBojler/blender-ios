/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GPU_context.hh"

#include "GHOST_C-api.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace blender::gpu {

class GPUWorker {
 private:
  std::unique_ptr<std::thread> thread_;
  std::condition_variable condition_var_;
  std::mutex mutex_;
  std::atomic_bool terminate_ = false;

 public:
  GPUWorker(std::function<void()> run_cb);
  ~GPUWorker();

  void wake_up()
  {
    condition_var_.notify_one();
  }

 private:
  void run(GPUContext *blender_gpu_context,
           GHOST_ContextHandle ghost_gpu_context,
           std::function<void()> run_cb);
};

}  // namespace blender::gpu
