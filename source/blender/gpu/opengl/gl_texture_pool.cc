/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gl_backend.hh"
#include "gl_texture.hh"
#include "gl_texture_pool.hh"

namespace blender::gpu {

GLTexturePool::~GLTexturePool()
{
  for (auto &handle : acquired_) {
    release_texture(wrap(handle.texture));
  }
  for (auto &handle : pool_) {
    /* TODO(not_mark): implement. */
    // handle.free();
  }
}

Texture *GLTexturePool::acquire_texture(int2 extent,
                          TextureFormat format,
                          eGPUTextureUsage usage)
{
  char name[16] = "TexFromPool";
  if (G.debug & G_DEBUG_GPU) {
    int texture_id = acquired_.size();
    SNPRINTF(name, "TexFromPool_%d", texture_id);
  }
  
  /* Define requirements for backing texture. */

  /* Search for the first compatible existing texture. */

  /* Acquire the compatible texture, or create a new one as a last resort. */

  /* Create a texture view. */

  return nullptr;
}

void GLTexturePool::release_texture(Texture *tex)
{
  auto texture_handle = acquired_.lookup_key({unwrap(tex), {}, 1});

}

void GLTexturePool::reset(bool force_free)
{
#ifndef NDEBUG
  /* Iterate acquired textures, and ensure the internal counter equals 0; otherwise
   * this indicates a missing `::retain()` or `::release()`. */
  for (const TextureHandle &tex : acquired_) {
    BLI_assert_msg(tex.counter == 0,
                   "Missing texture release/retain. Likely TextureFromPool::release(), "
                   "TextureFromPool::retain() or TexturePool::release_texture().");
  }
#endif

  /* TODO(not_mark): reverse iterator here. */
}

void GLTexturePool::offset_texture_counter(Texture *tex, int offset)
{

}

}  // namespace blender::gpu
