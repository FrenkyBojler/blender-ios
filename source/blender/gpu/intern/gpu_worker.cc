/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GPU_worker.hh"

#include "BKE_global.hh"
#include "BLI_threads.h"
#include "DNA_userdef_types.h"

namespace blender::gpu {

static GHOST_TDrawingContextType ghost_context_type()
{
  switch (GPU_backend_type_selection_get()) {
#ifdef WITH_OPENGL_BACKEND
    case GPU_BACKEND_OPENGL:
      return GHOST_kDrawingContextTypeOpenGL;
#endif
#ifdef WITH_VULKAN_BACKEND
    case GPU_BACKEND_VULKAN:
      return GHOST_kDrawingContextTypeVulkan;
#endif
#ifdef WITH_METAL_BACKEND
    case GPU_BACKEND_METAL:
      return GHOST_kDrawingContextTypeMetal;
#endif
    default:
      BLI_assert_unreachable();
      return GHOST_kDrawingContextTypeNone;
  }
}

GPUWorker::GPUWorker(std::function<void()> run_cb)
{
  BLI_assert(BLI_thread_is_main());

  GPUContext *main_thread_context = GPU_context_active_get();

  /* GPU settings for context creation. */
  GHOST_GPUSettings gpu_settings = {0};
  gpu_settings.context_type = ghost_context_type();
  if (G.debug & G_DEBUG_GPU) {
    gpu_settings.flags |= GHOST_gpuDebugContext;
  }
  gpu_settings.preferred_device.index = U.gpu_preferred_index;
  gpu_settings.preferred_device.vendor_id = U.gpu_preferred_vendor_id;
  gpu_settings.preferred_device.device_id = U.gpu_preferred_device_id;

  /* Grab the system handle.  */
  GHOST_SystemHandle ghost_system = reinterpret_cast<GHOST_SystemHandle>(
      GPU_backend_ghost_system_get());
  BLI_assert(ghost_system);

  /* Create a Ghost GPU Context using the system handle. */
  GHOST_ContextHandle ghost_gpu_context = GHOST_CreateGPUContext(ghost_system, gpu_settings);
  BLI_assert(ghost_gpu_context);

  /* Create a GPU context for the compile thread to use. */
  GPUContext *thread_context = GPU_context_create(nullptr, ghost_gpu_context);
  BLI_assert(thread_context);

  /* Create a new thread */
  thread_ = std::make_unique<std::thread>([this, thread_context, ghost_gpu_context, run_cb]() {
    this->run(thread_context, ghost_gpu_context, run_cb);
  });

  /* Restore the main thread context.
   * (required as the above context creation also makes it active). */
  GPU_context_active_set(main_thread_context);
}

GPUWorker::~GPUWorker()
{
  terminate_ = true;
  condition_var_.notify_one();
  thread_->join();
}

void GPUWorker::run(GPUContext *blender_gpu_context,
                    GHOST_ContextHandle ghost_gpu_context,
                    std::function<void()> run_cb)
{
  /* Contexts can only be created on the main thread so we have to
   * pass one in and make it active here. */
  GHOST_ActivateGPUContext(ghost_gpu_context);

  GPU_context_active_set(blender_gpu_context);

  /* Loop until we get the terminate signal. */
  while (!terminate_) {
    /* Wait until wake_up() */
    std::unique_lock<std::mutex> lock(mutex_);
    condition_var_.wait(lock);
    if (terminate_) {
      continue;
    }

    run_cb();
  }

  GPU_context_discard(blender_gpu_context);

  GHOST_ReleaseGPUContext(ghost_gpu_context);

  GHOST_SystemHandle ghost_system = reinterpret_cast<GHOST_SystemHandle>(
      GPU_backend_ghost_system_get());
  BLI_assert(ghost_system);
  GHOST_DisposeGPUContext(ghost_system, ghost_gpu_context);
}

}  // namespace blender::gpu
