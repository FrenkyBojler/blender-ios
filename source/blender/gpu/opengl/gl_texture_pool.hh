/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_texture_pool_private.hh"

namespace blender::gpu {

class GLTexturePool : public TexturePool {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  struct AllocationHandle {
    GLTexture *texture = nullptr;
    int counter = 0;
  };

  struct TextureHandle {
    GLTexture *texture_allocation = nullptr; /* Optional actual texture behind view. */
    GLTexture *texture = nullptr; /* Either created texture, or aliasing view over texture. */
    bool is_texture_view;
    int counter = 1;

    /* We use the pointer as hash/comparator, as a TextureHandle cannot be acquired twice.
     * This means we can find the handle without knowing the internal counter. */
    uint64_t hash() const
    {
      return get_default_hash(texture);
    }

    bool operator==(const TextureHandle &o) const
    {
      return texture == o.texture;
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
  void offset_texture_counter(Texture *tex, int offset) override;
};

}  // namespace blender::gpu
