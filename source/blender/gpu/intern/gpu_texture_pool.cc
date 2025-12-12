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
  for (TextureHandle &tex : acquired_) {
    GPU_texture_free(tex.texture);
  }
  for (TextureHandle &tex : pool_) {
    GPU_texture_free(tex.texture);
  }
}

Texture *TexturePool::acquire_texture(int width,
                                      int height,
                                      TextureFormat format,
                                      eGPUTextureUsage usage)
{
  /* Search pool for compatible released texture first. */
  int64_t match_index = -1;
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

  /* If compatible released texture was found, acquire and return it. */
  if (match_index != -1) {
    Texture *tex = pool_[match_index].texture;
    acquired_.append({tex, init_acquire_cycles});
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
  Texture *tex = GPU_texture_create_2d(name, width, height, 1, format, usage, nullptr);
  acquired_.append({tex, init_acquire_cycles});

  std::printf("Allocated %d\n", pool_.size() + acquired_.size());

  return tex;
}

bool TexturePool::is_texture_acquired(Texture *tex) const
{
  for (const auto &handle : acquired_) {
    if (handle.texture == tex) {
      return true;
    }
  }
  return false;
}

void TexturePool::release_texture(Texture *tex)
{
  /* Search for matching index of texture. */
  int64_t index = -1;
  for (auto i : acquired_.index_range()) {
    if (acquired_[i].texture == tex) {
      index = i;
      break;
    }
  }

  /* ::release_texture() is safe, as textures are sometimes released multiple 
   * times or were already released by the pool prematurely. */
  if (index != -1) {
    /* Move texture from acquired to pool. */
    pool_.append({tex, init_pool_cycles});
    acquired_.remove_and_reorder(index);
  }
}

void TexturePool::retain_texture(Texture *tex)
{
  /* Search for matching index of texture. */
  int64_t index = -1;
  for (auto i : acquired_.index_range()) {
    if (acquired_[i].texture == tex) {
      index = i;
      break;
    }
  }

  BLI_assert_msg(index != -1, "Unacquired texture retain in TexturePool.retain_texture()");
  
  /* `::reset()` will release when `remaining_cycles` reaches `max_acquire_cycles`. */
  acquired_[index].remaining_cycles = 0; 
}

void TexturePool::report(Texture *tex)
{
  /* Search for matching index of texture. */
  int64_t index = -1;
  for (auto i : acquired_.index_range()) {
    if (acquired_[i].texture == tex) {
      index = i;
      break;
    }
  }
  BLI_assert_msg(index != -1, "Unacquired texture report in TexturePool.report()");

  std::printf("tex %d has counter %d\n", index, acquired_[index].remaining_cycles);
}

void TexturePool::reset(bool force_free)
{
  std::printf("Reset pre (pool=%d, acquired=%d)\n", pool_.size(), acquired_.size());

  /* Reverse iterate pool textures, to make sure we only reorder known good handles. */
  for (int i = pool_.size() - 1; i >= 0; i--) {
    TextureHandle &tex = pool_[i];
    if (tex.remaining_cycles >= max_pool_cycles || force_free) {
      GPU_texture_free(tex.texture);
      pool_.remove_and_reorder(i);
    }
    else {
      tex.remaining_cycles++;
    }
  }

  /* Reverse iterate acquired textures, to make sure we only reorder known good handles.
   * Check to release textures that have not been retained for several cycles, and assert
   * on memory leaks. */
  for (int i = acquired_.size() - 1; i >= 0; i--) {
    TextureHandle &tex = acquired_[i];

    BLI_assert_msg(tex.remaining_cycles != -1,
                   "Missing texture release/retain. Either TextureFromPool.release() or "
                   "TexturePool.release_texture()");
                   
    if (tex.remaining_cycles >= max_acquire_cycles || force_free) {
      pool_.append({tex.texture, init_pool_cycles});
      acquired_.remove_and_reorder(i);
      std::printf("Release persistent texture=%d\n", i);
    }
    else {
      tex.remaining_cycles++;
    }
  }

  std::printf("Reset post (pool=%d, acquired=%d)\n", pool_.size(), acquired_.size());
}

void TexturePool::swap_texture_counters(Texture *a, Texture *b)
{
  /* Search for matching indices of textures. */
  int64_t index_a = -1;
  int64_t index_b = -1;
  for (int64_t i : acquired_.index_range()) {
    if (acquired_[i].texture == a) {
      index_a = i;
    }
    if (acquired_[i].texture == b) {
      index_b = i;
    }
  }

  BLI_assert_msg(index_a != -1, "Unacquired texture `a` in TexturePool.swap_texture_counters()");
  BLI_assert_msg(index_b != -1, "Unacquired texture `b` in TexturePool.swap_texture_counters()");

  /* Swap internal counters only. */
  std::swap(acquired_[index_a].remaining_cycles, acquired_[index_b].remaining_cycles);
}

TexturePool &TexturePool::get()
{
  BLI_assert(GPU_context_active_get() != nullptr);
  return *unwrap(GPU_context_active_get())->texture_pool;
}

}  // namespace blender::gpu
