/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * A `gpu::TextureFromPool` is a wrapper around backend specific texture objects whose usage is
 * transient and can be shared between parts of an engine or across several parts of blender.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"
#include "GPU_texture.hh"

namespace blender::gpu {

struct TexturePool {
  virtual ~TexturePool() = default;

  /* Return the texture pool from the active GPUContext.
   * Only valid if a context is active. */
  static TexturePool &get();

  /* Acquire a 2D texture from the pool with the given characteristics. */
  virtual Texture *acquire_texture(int2 extent,
                                   TextureFormat format,
                                   eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL,
                                   eGPUTextureLifetime lifetime = GPU_TEXTURE_LIFETIME_TRANSIENT) = 0;

  /* Release the texture back into the pool so it can be reused. */
  virtual void release_texture(Texture *tmp_tex) = 0;

  /* Validate acquired texture counters and release unused textures.
   * If `force_free` is true, free unused texture memory inside the pool. */
  virtual void reset(bool force_free = false) = 0;

  /* Modify the internal counter of an acquired texture.
   * Used by `TextureFromPool::retain()` in `DRW_gpu_wrapper.hh`. */
  virtual void offset_texture_counter(Texture *tex, int offset) = 0;
};

/**
 * Old texture pool implementation, to be used while a backend-specific
 * implementation is not yet available.
 */
class TexturePoolImpl : public TexturePool {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  /* Internal packet for texture, which supports set insertion. */
  struct TextureHandle {
    Texture *texture;
    /* Counter to track texture acquire/retain mismatches in `acquire_`.  */
    int users_count = 1;
    /* Counter to track the number of unused cycles before deallocation in `pool_`. */
    int unused_cycles_count = 0;

    /* We use the pointer as hash/comparator, as a texture cannot be acquired twice. */
    inline uint64_t hash() const
    {
      return get_default_hash(texture);
    }

    inline bool operator==(const TextureHandle &o) const
    {
      return texture == o.texture;
    }
  };

  /* Pool of textures ready to be reused. */
  Vector<TextureHandle> pool_;
  /* Set of textures currently in use. */
  Set<TextureHandle> acquired_;

public:
  ~TexturePoolImpl();

  /* Acquire a 2D texture from the pool with the given characteristics. */
  Texture *acquire_texture(int2 extent,
                           TextureFormat format,
                           eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL) override;

  void release_texture(Texture *tex) override;

  void reset(bool force_free = false) override;

  /* Modify the internal counter of an acquired texture.
   * Used by `TextureFromPool::retain()` in `DRW_gpu_wrapper.hh`. */
  void offset_users_count(Texture *tex, int offset);
};

}  // namespace blender::gpu
