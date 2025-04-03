/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GPU_worker.hh"

#include "BKE_global.hh"
#include "BLI_assert.h"
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

GPUSecondaryContext::GPUSecondaryContext()
{
  /* Contexts can only be created on the main thread. */
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
  ghost_context_ = GHOST_CreateGPUContext(ghost_system, gpu_settings);
  BLI_assert(ghost_context_);

  GHOST_TSuccess success = GHOST_ActivateGPUContext(ghost_context_);
  BLI_assert(success);

  /* Create a GPU context for the secondary thread to use. */
  gpu_context_ = GPU_context_create(nullptr, ghost_context_);
  BLI_assert(gpu_context_);

  /* Restore the main thread context.
   * (required as the above context creation also makes it active). */
  GPU_context_active_set(main_thread_context);

  success = GHOST_ReleaseGPUContext(ghost_context_);
  BLI_assert(success);

  /* NOTE: GHOST context doesn't need to be restored since GHOST contexts are not activated on
   * creation. */
}

GPUSecondaryContext::~GPUSecondaryContext()
{
  /* Contexts should be destructe on the thread they were activated. */
  BLI_assert(!BLI_thread_is_main());

  GPU_context_discard(gpu_context_);

  GHOST_ReleaseGPUContext(ghost_context_);

  GHOST_SystemHandle ghost_system = reinterpret_cast<GHOST_SystemHandle>(
      GPU_backend_ghost_system_get());
  BLI_assert(ghost_system);
  GHOST_DisposeGPUContext(ghost_system, ghost_context_);
}

void GPUSecondaryContext::activate()
{
  /* Contexts need to be activated in the thread they're going to be used. */
  BLI_assert(!BLI_thread_is_main());

  GHOST_ActivateGPUContext(ghost_context_);
  GPU_context_active_set(gpu_context_);
}

GPUWorker::GPUWorker(uint32_t threads_count, bool share_context, std::function<void()> run_cb)
{
  std::shared_ptr<GPUSecondaryContext> shared_context = nullptr;
  if (share_context) {
    shared_context = std::make_shared<GPUSecondaryContext>();
  }

  for (int i : IndexRange(threads_count)) {
    UNUSED_VARS(i);
    std::shared_ptr<GPUSecondaryContext> thread_context =
        share_context ? shared_context : std::make_shared<GPUSecondaryContext>();
    threads_.append(std::make_unique<std::thread>([=]() { this->run(thread_context, run_cb); }));
  }
}

GPUWorker::~GPUWorker()
{
  terminate_ = true;
  condition_var_.notify_all();
  for (std::unique_ptr<std::thread> &thread : threads_) {
    thread->join();
  }
}

void GPUWorker::run(std::shared_ptr<GPUSecondaryContext> context, std::function<void()> run_cb)
{
  context->activate();

  /* Loop until we get the terminate signal. */
  while (!terminate_) {
    {
      /* Wait until wake_up() */
      std::unique_lock<std::mutex> lock(mutex_);
      condition_var_.wait(lock);
    }
    if (terminate_) {
      continue;
    }

    run_cb();
  }
}

}  // namespace blender::gpu
