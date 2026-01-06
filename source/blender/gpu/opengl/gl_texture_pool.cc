/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BLI_string.h"

#include "gl_backend.hh"
#include "gl_texture_pool.hh"

namespace blender::gpu {

static bool are_formats_compatible(const TextureFormat format_a, const TextureFormat format_b)
{
  GPUTextureFormatFlag flag_a = to_format_flag(format_a);
  GPUTextureFormatFlag flag_b = to_format_flag(format_b);
  bool is_depth_or_stencil =
      (flag_a & GPU_FORMAT_DEPTH_STENCIL) != 0 | (flag_b & GPU_FORMAT_DEPTH_STENCIL) != 0;
  bool is_compressed = 
      (flag_a & GPU_FORMAT_COMPRESSED) != 0 | (flag_b & GPU_FORMAT_COMPRESSED) != 0;

  /* glTextureView does not support depth/stencil formats, so we can only re-use
   * these textures if they share format. Compressed formats also have restricted
   * aliasing capabilities with glTextureView. */
  if (is_depth_or_stencil || is_compressed) {
    return format_a == format_b;
  }

  /* WATCHME(not_mark): this might not be exhaustive. */
  return to_bytesize(format_a) == to_bytesize(format_b);
}

GLTexturePool::~GLTexturePool()
{
  for (const auto &handle : acquired_) {
    release_texture(wrap(handle.texture));
  }
  for (auto &handle : pool_) {
    GPU_texture_free(handle.texture);
  }
}

Texture *GLTexturePool::acquire_texture(int2 extent, TextureFormat format, eGPUTextureUsage usage)
{
  char name[16] = "TexFromPool";
  if (G.debug & G_DEBUG_GPU) {
    int texture_id = acquired_.size();
    SNPRINTF(name, "TexFromPool_%d", texture_id);
  }

  /* Search for the first compatible existing texture. */
  int64_t match_index = -1;
  for (uint64_t i : pool_.index_range()) {
    const auto &handle = pool_[i];
    if (int2(handle.texture->w_, handle.texture->h_) != extent) {
      continue;
    }
    if (!are_formats_compatible(handle.texture->format_, format)) {
      continue;
    }
    match_index = i;
    break;
  }

  /* Acquire the compatible texture, or create a new one as a last resort. */
  GLTexture *texture_allocation;
  if (match_index != -1) {
    texture_allocation = pool_[match_index].texture;
    pool_.remove_and_reorder(match_index);
  }
  else {
    eGPUTextureUsage usage_flag = usage | GPU_TEXTURE_USAGE_FORMAT_VIEW;
    texture_allocation = unwrap(
        GPU_texture_create_2d(name, extent.x, extent.y, 1, format, usage_flag, nullptr));
  }

  /* Assemble texture handle, including `glTextureView`, if format aliasing is required. */
  TextureHandle texture_handle;
  if (texture_allocation->format_ == format) {
    texture_handle.texture_allocation = texture_handle.texture = texture_allocation;
  }
  else {
    texture_handle.texture_allocation = texture_allocation;
    texture_handle.texture = unwrap(
        GPU_texture_create_view(name, texture_allocation, format, 0, 1, 0, 1, false, false));
  }

  acquired_.add(texture_handle);
  return wrap(texture_handle.texture);
}

void GLTexturePool::release_texture(Texture *tex)
{
  BLI_assert_msg(acquired_.contains({unwrap(tex)}),
                 "Unacquired texture passed to TexturePool::offset_users_count()");
  auto texture_handle = acquired_.lookup_key({unwrap(tex), {}, 1});

  /* Move allocation back to `pool_`. */
  AllocationHandle allocation_handle;
  allocation_handle.texture = texture_handle.texture_allocation;
  pool_.append(allocation_handle);

  /* Destroy view, if one was created. */
  if (texture_handle.is_view()) {
    GPU_texture_free(texture_handle.texture);
  }
  acquired_.remove(texture_handle);
}

void GLTexturePool::offset_users_count(Texture *tex, int offset)
{
  BLI_assert_msg(acquired_.contains({unwrap(tex)}),
                 "Unacquired texture passed to TexturePool::offset_users_count()");
  auto texture_handle = acquired_.lookup_key({unwrap(tex), {}, 1});
  texture_handle.users_count += offset;
  acquired_.add_overwrite(texture_handle);
}

void GLTexturePool::reset(bool force_free)
{
#ifndef NDEBUG
  /* Iterate acquired textures, and ensure the internal counter equals 0; otherwise
   * this indicates a missing `::retain()` or `::release()`. */
  for (const TextureHandle &tex : acquired_) {
    BLI_assert_msg(tex.users_count == 0,
                   "Missing texture release/retain. Likely TextureFromPool::release(), "
                   "TextureFromPool::retain() or TexturePool::release_texture().");
  }
#endif

  /* Reverse iterate unused allocations, to make sure we only reorder known good handles. */
  for (int i = pool_.size() - 1; i >= 0; i--) {
    AllocationHandle &handle = pool_[i];
    if (handle.unused_cycles_count >= max_unused_cycles_ || force_free) {
      GPU_texture_free(handle.texture);
      pool_.remove_and_reorder(i);
    }
    else {
      handle.unused_cycles_count++;
    }
  }
}
}  // namespace blender::gpu
