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

#include "BLI_vector.hh"

#include "GPU_texture.hh"

namespace blender::gpu {

class TexturePool {
 private:
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles = 8;

  /* Associated counter is used to track texture acquire/retain, or
   * nr. of unused cycles before deallocation. */
  struct TextureHandle {
    Texture *texture;
    int counter;
  };

  /* Pool of textures ready to be reused. */
  Vector<TextureHandle> pool_;
  /* List of textures currently in use. */
  Vector<TextureHandle> acquired_;

 public:
  ~TexturePool();

  /* Return the texture pool from the active GPUContext.
   * Only valid if a context is active. */
  static TexturePool &get();

  /* Acquire a texture from the pool with the given characteristics. */
  Texture *acquire_texture(int width,
                           int height,
                           TextureFormat format,
                           eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL);

  /* Release the texture back into the pool so it can be reused. */
  void release_texture(Texture *tex);

  /* Decrease acquired texture counters and release/invalidate unused textures.
   * If `force_free` is true, free unused texture memory inside the pool. */
  void reset(bool force_free = false);

  /* Reference the internal counter of an acquired texture.
   * Used by `TextureFromPool` in `DRW_gpu_wrapper.hh`. */
  int &get_texture_counter(Texture *tex);
};

}  // namespace blender::gpu
