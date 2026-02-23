/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "COM_static_cache_manager.hh"

class GHOST_IContext;

namespace blender::seq {

class CompositorCache {
 private:
  compositor::StaticCacheManager cache_manager;
  bool last_evaluation_used_gpu = false;
  compositor::ResultPrecision last_evaluation_precision = compositor::ResultPrecision::Half;
  GHOST_IContext *last_evaluation_ghost_context = nullptr;

 public:
  ~CompositorCache();

  compositor::StaticCacheManager &get_cache_manager()
  {
    return cache_manager;
  }

  void recreate_if_needed(bool gpu,
                          compositor::ResultPrecision precision,
                          GHOST_IContext *ghost_context);
};

}  // namespace blender::seq
