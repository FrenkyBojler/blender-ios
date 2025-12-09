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

/* Explicit lifetime hints for texture pool resources. Transient textures must be handed back in
 * the same cycle, while persistent textures must be handed back before a full reset. */
enum eTextureLifetime { TEXTURE_LIFETIME_TRANSIENT, TEXTURE_LIFETIME_PERSISTENT };

namespace blender::gpu {

class TexturePool {
 private:
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  struct TextureHandle {
    Texture *texture;
    /* Counts the number of `reset()` call since the last use.
     * The texture memory is deallocated after a certain number of cycles. */
    int unused_cycles;
  };

  /* Pool of texture ready to be reused. */
  Vector<TextureHandle> pool_;
  /* List of textures that are currently being used. Tracked to check memory leak. */
  Vector<gpu::Texture *> acquired_;

 public:
  ~TexturePool();

  /* Return the texture pool from the active GPUContext.
   * Only valid if a context is active. */
  static TexturePool &get();

  /* TODO(not_mark): impl. overloads for at least 2D, 2D array. */
  Texture *acquire_texture_2d(int2 extent,
                              TextureFormat format,
                              eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL,
                              eTextureLifetime lifetime = TEXTURE_LIFETIME_TRANSIENT);
  Texture *acquire_texture_2d_array(int2 extent,
                                    int layers,
                                    TextureFormat format,
                                    eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL,
                                    eTextureLifetime lifetime = TEXTURE_LIFETIME_TRANSIENT);

  /* Acquire a texture from the pool with the given characteristics. */
  Texture *acquire_texture(int width,
                           int height,
                           TextureFormat format,
                           eGPUTextureUsage usage,
                           eTextureLifetime lifetime);

  /* Release the texture so that its memory can be reused at some other point. */
  void release_texture(Texture *tmp_tex);

  /* Switch lifetime of a texture from/to transient to/form persistent. */
  void make_texture_persistent(Texture *tex);
  void make_texture_transient(Texture *tex);

  /* Ensure no texture is still acquired and release unused textures.
   * If `force_free` is true, free all the texture memory inside the pool.
   * Otherwise, only unused textures will be freed. */
  void reset(bool force_free = false);
};

}  // namespace blender::gpu
