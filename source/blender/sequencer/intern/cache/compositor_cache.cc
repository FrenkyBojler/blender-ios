/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_threads.h"
#include "COM_result.hh"
#include "DRW_engine.hh"
#include "WM_api.hh"

#include "compositor_cache.hh"

namespace blender::seq {

CompositorCache::~CompositorCache()
{
  bool use_main_context = false;
  if (this->last_evaluation_used_gpu) {
    /* Free resources with GPU context enabled. Cleanup may happen from the main thread, and we
     * must use the main context there. */
    use_main_context = BLI_thread_is_main() || this->last_evaluation_ghost_context == nullptr;
    if (use_main_context) {
      DRW_gpu_context_enable();
    }
    else {
      WM_system_gpu_context_activate(last_evaluation_ghost_context);
    }
  }

  this->cache_manager.free();

  /* See comment above on context enabling. */
  if (this->last_evaluation_used_gpu) {
    if (use_main_context) {
      DRW_gpu_context_disable();
    }
    else {
      WM_system_gpu_context_release(last_evaluation_ghost_context);
    }
  }
}

void CompositorCache::recreate_if_needed(bool gpu,
                                         compositor::ResultPrecision precision,
                                         GHOST_IContext *ghost_context)
{
  this->last_evaluation_ghost_context = gpu ? ghost_context : nullptr;

  if (this->last_evaluation_used_gpu == gpu && this->last_evaluation_precision == precision) {
    return;
  }
  this->cache_manager.free();
  this->last_evaluation_used_gpu = gpu;
  this->last_evaluation_precision = precision;
}

}  // namespace blender::seq
