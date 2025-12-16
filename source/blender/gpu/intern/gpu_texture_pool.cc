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

  /* If compatible pool texture was found, acquire and return it. */
  if (match_index != -1) {
    Texture *tex = pool_[match_index].texture;
    acquired_.append({tex, 1}); /* Internal counter set to 1 on acquire. */
    pool_.remove_and_reorder(match_index);
    std::printf("ACQUIRE %p\n", tex);
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
  acquired_.append({tex, 1}); /* Internal counter set to 1 on acquire. */
  
  return tex;
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

  if (index == -1) {
    return;
  }
  // BLI_assert_msg(index != -1, 
  //                "Unacquired texture passed to TexturePool::release_texture()");

  /* Move texture from acquired to pool. */
  pool_.append({tex, 0});
  acquired_.remove_and_reorder(index);
}

bool TexturePool::is_texture_acquired(Texture *tex) const
{
  /* Search for matching index of texture. */
  int64_t index = -1;
  for (auto i : acquired_.index_range()) {
    if (acquired_[i].texture == tex) {
      index = i;
      break;
    }
  }

  return index != -1;
}

int &TexturePool::get_texture_counter(Texture *tex)
{
  /* Search for matching index of texture. */
  int64_t index = -1;
  for (auto i : acquired_.index_range()) {
    if (acquired_[i].texture == tex) {
      index = i;
      break;
    }
  }

  BLI_assert_msg(index != -1,
                 "Unacquired texture passed to TexturePool::get_texture_counter()");

  return acquired_[index].counter;
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
  // std::printf("Swapping counters: %d - %d\n", acquired_[index_a].counter, acquired_[index_b].counter);
  std::swap(acquired_[index_a].counter, acquired_[index_b].counter);
}

void TexturePool::reset(bool force_free)
{
  /* Iterate acquired textures, and ensure `TextureHandle::counter` equals 0; otherwise
   * this indicates a missing `::retain()` or `::release()` of a texture. */
  for (int i = acquired_.size() - 1; i >= 0; i--) {
    TextureHandle &tex = acquired_[i];

    BLI_assert_msg(tex.counter == 0,
                   "Missing texture release/retain. Likely TextureFromPool::release(), "
                   "TextureFromPool::retain() or TexturePool::release_texture().");

    // /* On `force_free`, acquired textures are forcibly invalidated. */
    // if (force_free) {
    //   pool_.append({tex.texture, 0});
    //   acquired_.remove_and_reorder(i);
    //   std::printf("remove_and_reorder on acquired texture\n");
    // }
  }

  /* Reverse iterate pool textures, to make sure we only reorder known good handles. */
  for (int i = pool_.size() - 1; i >= 0; i--) {
    TextureHandle &tex = pool_[i];
    if (tex.counter >= max_unused_cycles || force_free) {
      GPU_texture_free(tex.texture);
      pool_.remove_and_reorder(i);
    }
    else {
      tex.counter++;
    }
  }
}

TexturePool &TexturePool::get()
{
  BLI_assert(GPU_context_active_get() != nullptr);
  return *unwrap(GPU_context_active_get())->texture_pool;
}

}  // namespace blender::gpu
