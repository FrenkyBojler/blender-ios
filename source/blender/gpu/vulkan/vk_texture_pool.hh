/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_texture_pool_private.hh"

namespace blender::gpu {

class VKTexturePool : public TexturePool {
  /* ... */

 public:
  VKTexturePool();
  ~VKTexturePool();

  Texture *acquire_texture(int2 extent,
                           TextureFormat format,
                           eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL) final;

  void release_texture(Texture *tmp_tex) final;

  void reset(bool force_free = false) final;

  void offset_texture_counter(Texture *tex, int offset) final;
};

}  // namespace blender::gpu
