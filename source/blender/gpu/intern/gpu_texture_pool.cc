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
  for (TextureHandle tex : acquired_) {
    GPU_texture_free(tex.texture);
  }
  for (TextureHandle tex : pool_) {
    GPU_texture_free(tex.texture);
  }
}

Texture *TexturePool::acquire_texture(int2 extent, TextureFormat format, eGPUTextureUsage usage)
{
  /* Search pool for compatible released texture first. */
  int64_t match_index = -1;
  for (uint64_t i : pool_.index_range()) {
    Texture *tex = pool_[i].texture;
    /* TODO(@fclem): We could reuse texture using texture views if the formats are compatible. */
    if ((GPU_texture_format(tex) == format) && (GPU_texture_width(tex) == extent.x) &&
        (GPU_texture_height(tex) == extent.y) && (GPU_texture_usage(tex) == usage))
    {
      match_index = i;
      break;
    }
  }

  /* If compatible pool texture was found, acquire and return it. */
  if (match_index != -1) {
    Texture *tex = pool_[match_index].texture;
    acquired_.add({tex, 1}); /* Internal counter set to 1 on acquire. */
    pool_.remove_and_reorder(match_index);
    return tex;
  }

  /* Otherwise, allocate a new texture as a new resort. */
  /* TODO(@fclem): Rename each allocation using texture views. */
  char name[16] = "TexFromPool";
  if (G.debug & G_DEBUG_GPU) {
    int texture_id = pool_.size();
    SNPRINTF(name, "TexFromPool_%d", texture_id);
  }
  Texture *tex = GPU_texture_create_2d(name, UNPACK2(extent), 1, format, usage, nullptr);
  acquired_.add({tex, 1}); /* Internal counter set to 1 on acquire. */

  return tex;
}

void TexturePool::release_texture(Texture *tex)
{
  pool_.append({tex, 0});
  BLI_assert_msg(acquired_.remove({tex, 1}),
                 "Unacquired texture passed to TexturePool::release_texture()");
}

void TexturePool::offset_texture_counter(Texture *tex, int offset)
{
  int counter = acquired_.lookup_key({tex, 0}).counter;
  acquired_.add_overwrite({tex, counter + offset});
}

void TexturePool::reset(bool force_free)
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

  /* Reverse iterate pool textures, to make sure we only reorder known good handles. */
  for (int i = pool_.size() - 1; i >= 0; i--) {
    TextureHandle &tex = pool_[i];
    if (tex.counter >= max_unused_cycles_ || force_free) {
      GPU_texture_free(tex.texture);
      pool_.remove_and_reorder(i);
    }
    else {
      tex.counter++;
    }
  }

  std::printf("acquired=%d, pool=%d\n", acquired_.size(), pool_.size());
}

TexturePool &TexturePool::get()
{
  BLI_assert(GPU_context_active_get() != nullptr);
  return *unwrap(GPU_context_active_get())->texture_pool;
}

}  // namespace blender::gpu
