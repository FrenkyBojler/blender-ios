/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gl_texture.hh"
#include "gpu_texture_pool_private.hh"

namespace blender::gpu {

class GLTexturePool : public TexturePool {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  struct AllocationHandle {
    GLTexture *texture = nullptr;
    /* Counter to track the number of unused cycles before deallocation in `pool_`. */
    int unused_cycles_count = 0;
  };

  struct TextureHandle {
    /* Either created texture, or aliasing view over texture. */
    GLTexture *texture = nullptr;
    /* Optional backing texture behind view. */
    GLTexture *texture_allocation = nullptr; 
    /* Counter to track texture acquire/retain mismatches in `acquire_`.  */
    int users_count = 1;

    /* We use the pointer as hash/comparator, as a texture cannot be acquired twice. */
    uint64_t hash() const
    {
      return get_default_hash(texture);
    }

    bool operator==(const TextureHandle &o) const
    {
      return texture == o.texture;
    }

    bool is_view() const
    {
      return texture != nullptr && texture != texture_allocation;
    }
  };

  Vector<AllocationHandle> pool_;
  Set<TextureHandle> acquired_;

 public:
  ~GLTexturePool();
  
  Texture *acquire_texture(int2 extent,
                           TextureFormat format,
                           eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL) override;

  void release_texture(Texture *tex) override;

  void reset(bool force_free = false) override;

  void offset_users_count(Texture *tex, int offset) override;
};

}  // namespace blender::gpu
