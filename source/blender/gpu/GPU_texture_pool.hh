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
  static constexpr int max_unused_cycles_ = 8;
  /* On `retain`, a texture remains acquired for enough cycles that it can survive the 
   * multiple reset calls until the next frame. */
  static constexpr int max_retain_cycles_ = 4; /* FIXME(not_mark): this is REALLY FLAKY AAAARGH */
  /* Textures are stored with a counter, counting down the number of `reset` calls
   * since last use. Depending on the context:
   * - For `pool_` handles, the texture is deallocated once it reaches 0.
   *   The initial value is `max_unused_cycles`.
   * - For `acquired_` handles, the texture is released if it reaches 0.
   * - For `acquired_` handles, an error is thrown if it reaches -1, as
   *   a texture was not retained/released, causing a memory leak. */
  struct TextureHandle {
    Texture *texture;
    int remaining_cycles;
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

  /* Check if a pointer is associated with an acquired texture. */
  bool is_texture_acquired(Texture *tex) const;

  /* Release the texture back into the pool so it can be reused. */
  void release_texture(Texture *tex);

  /* Indicate that the texture should survive into the next `max_retain_cycles_` cycles. */
  void retain_texture(Texture *tex);

  /* Decrease acquired texture counters and release/invalidate unused textures.
   * If `force_free` is true, free all the texture memory inside the pool.
   * Otherwise, only unused textures will be freed. */
  void reset(bool force_free = false);
  
  /* Swap lifetime counters of two acquired textures, enabling e.g. a single-frame
   * texture and retained texture to exchange values. */
  void swap_texture_counters(Texture *a, Texture *b);

  void report(Texture *tex);
};

}  // namespace blender::gpu
