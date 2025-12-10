/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "BKE_global.hh"
#include "BLI_string.h"

#include "GPU_texture_pool.hh"

#include "gpu_context_private.hh"

namespace blender::gpu {

TexturePool::~TexturePool()
{
  for (Texture *tex : acquired_transient_) {
    GPU_texture_free(tex);
  }
  for (Texture *tex : acquired_persistent_) {
    GPU_texture_free(tex);
  }
  for (TextureHandle &tex : pool_) {
    GPU_texture_free(tex.texture);
  }
}

Texture *TexturePool::acquire_texture(
    int width, int height, TextureFormat format, eGPUTextureUsage usage, eTextureLifetime lifetime)
{
  int64_t match_index = -1;

  /* Search released texture first. */
  for (auto i : pool_.index_range()) {
    Texture *tex = pool_[i].texture;
    /* TODO(@fclem): We could reuse texture using texture views if the formats are compatible. */
    if ((GPU_texture_format(tex) == format) && (GPU_texture_width(tex) == width) &&
        (GPU_texture_height(tex) == height) && (GPU_texture_usage(tex) == usage))
    {
      match_index = i;
      break;
    }
  }

  if (match_index != -1) {
    Texture *tex = pool_[match_index].texture;
    pool_.remove_and_reorder(match_index);

    if (lifetime == TEXTURE_LIFETIME_TRANSIENT) {
      acquired_transient_.append(tex);
    }
    else { /* TEXTURE_LIFETIME_PERSISTENT */
      acquired_persistent_.append(tex);
    }

    return tex;
  }

  /* Create a new texture in last resort. */
  /* TODO(@fclem): Rename each allocation using texture views. */
  char name[16] = "TexFromPool";
  if (G.debug & G_DEBUG_GPU) {
    int texture_id = pool_.size();
    SNPRINTF(name, "TexFromPool_%d", texture_id);
  }
  Texture *tex = GPU_texture_create_2d(name, width, height, 1, format, usage, nullptr);

  if (lifetime == TEXTURE_LIFETIME_TRANSIENT) {
    acquired_transient_.append(tex);
  }
  else { /* TEXTURE_LIFETIME_PERSISTENT */
    acquired_persistent_.append(tex);
  }

  return tex;
}

void TexturePool::release_texture(Texture *tex)
{
  if (int idx = acquired_transient_.first_index_of_try(tex); idx != -1) {
    acquired_transient_.remove_and_reorder(idx);
  }
  else if (int idx = acquired_persistent_.first_index_of_try(tex); idx != -1) {
    acquired_persistent_.remove_and_reorder(idx);
  }
  else {
    BLI_assert_msg(false, "Unacquired texture release in TexturePool.release_texture().");
  }
  pool_.append({tex, 0});
}

void TexturePool::make_texture_transient(Texture *tex)
{
  int idx = acquired_persistent_.first_index_of_try(tex);
  BLI_assert_msg(idx != -1, "Incorrect texture passed to TexturePool.make_texture_transient().");
  acquired_persistent_.remove_and_reorder(idx);
  acquired_transient_.append(tex);
}

void TexturePool::make_texture_persistent(Texture *tex)
{
  int idx = acquired_transient_.first_index_of_try(tex);
  BLI_assert_msg(idx != -1, "Incorrect texture passed to TexturePool.make_texture_persistent().");
  acquired_transient_.remove_and_reorder(idx);
  acquired_persistent_.append(tex);
}

bool TexturePool::is_texture_transient(Texture *tex) const
{
  return tex != nullptr && acquired_transient_.contains(tex);
}

bool TexturePool::is_texture_persistent(Texture *tex) const
{
  return tex != nullptr && acquired_persistent_.contains(tex);
}

void TexturePool::reset(bool force_free)
{
  BLI_assert_msg(acquired_transient_.is_empty(),
                 "Missing transient texture release. Either TextureFromPool.release() or "
                 "TexturePool.release_texture()");

  /* Clear out persistent texture pool on `force_free`.
   * Any use of `draw::TextureFromPoolPersistent::ensure_*()` will re-acquire. */
  if (force_free) {
    for (Texture *tex : acquired_persistent_) {
      GPU_texture_free(tex);
    }
    acquired_persistent_.clear();
  }

  /* Reverse iteration to make sure we only reorder with known good handles. */
  for (int i = pool_.size() - 1; i >= 0; i--) {
    TextureHandle &tex = pool_[i];
    if (tex.unused_cycles >= max_unused_cycles_ || force_free) {
      GPU_texture_free(tex.texture);
      pool_.remove_and_reorder(i);
    }
    else {
      tex.unused_cycles++;
    }
  }
}

TexturePool &TexturePool::get()
{
  BLI_assert(GPU_context_active_get() != nullptr);
  return *unwrap(GPU_context_active_get())->texture_pool;
}

}  // namespace blender::gpu
