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

class TexturePool {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  /* Internal packet for texture with and associated counter that supports `Set` insertion.
   * The counter tracks texture acquire/retain mismatches in `acquire_`, or the number
   * of unused cycles before deallocation in `pool_`. */
  struct TextureHandle {
    Texture *texture;
    int counter;

    /* We use the pointer as hash/comparator, as a TextureHandle cannot be acquired twice.
     * This means we can find the handle without knowing the internal counter. */
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
  ~TexturePool();

  /* Return the texture pool from the active GPUContext.
   * Only valid if a context is active. */
  static TexturePool &get();

  /* Acquire a 2D texture from the pool with the given characteristics. */
  Texture *acquire_texture(int2 extent,
                           TextureFormat format,
                           eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL);

  /* Release the texture back into the pool so it can be reused. */
  void release_texture(Texture *tex);

  /* Validate acquired texture counters and release unused textures.
   * If `force_free` is true, free unused texture memory inside the pool. */
  void reset(bool force_free = false);

  /* Modify the internal counter of an acquired texture.
   * Used by `TextureFromPool::retain()` in `DRW_gpu_wrapper.hh`. */
  void offset_texture_counter(Texture *tex, int offset);
};

}  // namespace blender::gpu
