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
  for (GPUTexture *tex : acquired) {
    GPU_texture_free(tex);
  }
  for (TextureHandle &tex : pool) {
    GPU_texture_free(tex.texture);
  }
}

GPUTexture *TexturePool::acquire_texture(int width,
                                         int height,
                                         eGPUTextureFormat format,
                                         eGPUTextureUsage usage)
{
  int64_t match_index = -1;
  /* Search released texture first. */
  for (auto i : pool.index_range()) {
    GPUTexture *tex = pool[i].texture;
    /* TODO(@fclem): We could reuse texture using texture views if the formats are compatible. */
    if ((GPU_texture_format(tex) == format) && (GPU_texture_width(tex) == width) &&
        (GPU_texture_height(tex) == height) && (GPU_texture_usage(tex) == usage))
    {
      match_index = i;
      break;
    }
  }

  if (match_index != -1) {
    GPUTexture *tex = pool[match_index].texture;
    pool.remove_and_reorder(match_index);
    acquired.append(tex);
    return tex;
  }

  /* Create a new texture in last resort. */
  /* TODO(@fclem): Rename each allocation using texture views. */
  char name[16] = "TexFromPool";
  if (G.debug & G_DEBUG_GPU) {
    int texture_id = pool.size();
    SNPRINTF(name, "TexFromPool_%d", texture_id);
  }
  GPUTexture *tex = GPU_texture_create_2d(name, width, height, 1, format, usage, nullptr);
  acquired.append(tex);
  return tex;
}

void TexturePool::release_texture(GPUTexture *tex)
{
  acquired.remove_first_occurrence_and_reorder(tex);
  pool.append({tex, 0});
}

void TexturePool::take_texture_ownership(GPUTexture *tex)
{
  acquired.remove_first_occurrence_and_reorder(tex);
}

void TexturePool::give_texture_ownership(GPUTexture *tex)
{
  acquired.append(tex);
}

void TexturePool::reset(bool force_free)
{
  BLI_assert_msg(acquired.is_empty(),
                 "Missing texture release. Either TextureFromPool.release() or "
                 "TexturePool.release_texture()");

  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  const int max_unused_cycles = 8;
  /* Reverse iteration to make sure we only reorder with known good handles. */
  for (int i = pool.size() - 1; i >= 0; i--) {
    TextureHandle &tex = pool[i];
    if (tex.unused_cycles >= max_unused_cycles || force_free) {
      GPU_texture_free(tex.texture);
      pool.remove_and_reorder(i);
    }
    else {
      tex.unused_cycles++;
    }
  }
}

TexturePool &TexturePool::get()
{
  BLI_assert(GPU_context_active_get() != nullptr);
  return blender::gpu::unwrap(GPU_context_active_get())->texture_pool;
}

}  // namespace blender::gpu
