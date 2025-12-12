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
  /* Search pool for compatible texture first. */
  int64_t idx = -1;
  for (int64_t i : pool_.index_range()) {
    Texture *tex = pool_[i].texture;
    /* TODO(@fclem): We could reuse texture using texture views if the formats are compatible. */
    if ((GPU_texture_format(tex) == format) && (GPU_texture_width(tex) == width) &&
        (GPU_texture_height(tex) == height) && (GPU_texture_usage(tex) == usage))
    {
      idx = i;
      break;
    }
  }

  /* If matching texture was found, return it. */
  if (idx != -1) {
    Texture *tex = pool_[idx].texture;
    pool_.remove_and_reorder(idx);
    acquired_.append({tex, -1});
    return tex;
  }

  /* Otherwise, create a new texture. */
  /* TODO(@fclem): Rename each allocation using texture views. */
  char name[16] = "TexFromPool";
  if (G.debug & G_DEBUG_GPU) {
    int texture_id = pool_.size();
    SNPRINTF(name, "TexFromPool_%d", texture_id);
  }
  Texture *tex = GPU_texture_create_2d(name, width, height, 1, format, usage, nullptr);
  acquired_.append({tex, -1});

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
  int64_t idx = -1;
  for (int i : acquired_.index_range()) {
    if (acquired_[i].texture == tex) {
      idx = i;
      break;
    }
  }
  /* FIXME(not_mark): why not make release_texture() safe instead? */
  BLI_assert_msg(idx != -1, "Unacquired texture release in TexturePool.release_texture()");

  /* Move texture from acquired to pool. */
  pool_.append({tex, max_unused_cycles_});
  acquired_.remove_and_reorder(idx);
}

void TexturePool::retain_texture(Texture *tex)
{
  /* Search for matching index of texture. */
  int64_t idx = -1;
  for (int64_t i : acquired_.index_range()) {
    if (acquired_[i].texture == tex) {
      idx = i;
      break;
    }
  }

  BLI_assert_msg(idx != -1, "Unacquired texture retain in TexturePool.retain_texture()");
  
  acquired_[idx].remaining_cycles = max_retain_cycles_;
}

void TexturePool::report(Texture *tex)
{
  /* Search for matching index of texture. */
  int64_t idx = -1;
  for (int64_t i : acquired_.index_range()) {
    if (acquired_[i].texture == tex) {
      idx = i;
      break;
    }
  }
  BLI_assert_msg(idx != -1, "Unacquired texture report in TexturePool.report()");

  std::printf("tex %d has counter %d\n", idx, acquired_[idx].remaining_cycles);
}


void TexturePool::reset(bool force_free)
{
  /* Reverse iterate acquired textures, releasing those that should be invalidated,
   * and asserting if there is a missing texture release. */
  for (int i = acquired_.size() - 1; i >= 0; i--) {
    TextureHandle &tex = acquired_[i];

    BLI_assert_msg(tex.remaining_cycles >= 0,
                   "Missing texture release/retain. Either TextureFromPool.release() or "
                   "TexturePool.release_texture()");

    if (tex.remaining_cycles == 0 || force_free) {
      acquired_.remove_and_reorder(i);
      pool_.append({tex.texture, max_unused_cycles_});
      std::printf("Releasing early\n");
    }
    else {
      tex.remaining_cycles--;
    }
  }

  /* Reverse iterate pool textures, to make sure we only reorder known good handles. */
  for (int i = pool_.size() - 1; i >= 0; i--) {
    TextureHandle &tex = pool_[i];
    if (tex.remaining_cycles == 0 || force_free) {
      pool_.remove_and_reorder(i);
      GPU_texture_free(tex.texture);
    }
    else {
      tex.remaining_cycles--;
    }
  }
}

void TexturePool::swap_texture_counters(Texture *a, Texture *b)
{
  /* Search for matching indices of textures. */
  int64_t idx_a = -1;
  int64_t idx_b = -1;
  for (int64_t i : acquired_.index_range()) {
    if (acquired_[i].texture == a) {
      idx_a = i;
    }
    if (acquired_[i].texture == b) {
      idx_b = i;
    }
  }

  BLI_assert_msg(idx_a != -1, "Unacquired texture `a` in TexturePool.swap_texture_counters()");
  BLI_assert_msg(idx_b != -1, "Unacquired texture `b` in TexturePool.swap_texture_counters()");

  /* Swap internal counters only. */
  std::swap(acquired_[idx_a].remaining_cycles, acquired_[idx_b].remaining_cycles);
}

TexturePool &TexturePool::get()
{
  BLI_assert(GPU_context_active_get() != nullptr);
  return *unwrap(GPU_context_active_get())->texture_pool;
}

}  // namespace blender::gpu
