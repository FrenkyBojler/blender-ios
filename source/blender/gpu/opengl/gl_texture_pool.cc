/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BLI_string.h"

#include "gl_backend.hh"
#include "gl_texture_pool.hh"

#include "fmt/format.h"

namespace blender::gpu {

/* Given a TextureFormat of non-compressed, non-depth-stencil types, return a default
 * with which it can alias. The returned default has to be available in TextureWriteFormat,
 * TextureTargetFormat, and TextureFormat, as on some platforms (Intel), aliasing a
 * framebuffer-supporting format on a non-supporting format breaks framebuffer compatibility. */
static TextureFormat get_compatible_texture_format(TextureFormat format)
{
  GPUTextureFormatFlag format_flag = to_format_flag(format);

  if (bool(format_flag & GPU_FORMAT_DEPTH_STENCIL)) {
    return TextureFormat::Invalid;
  }
  if (bool(format_flag & GPU_FORMAT_COMPRESSED)) {
    return TextureFormat::Invalid;
  }

  switch (to_bytesize(format)) {
    case 16:
      return TextureFormat::SFLOAT_32_32_32_32;
    case 8:
      return TextureFormat::SFLOAT_32_32;
    case 4:
      return TextureFormat::SFLOAT_32;
    case 2:
      return TextureFormat::SFLOAT_16;
    case 1:
      return TextureFormat::UNORM_8;
    default:
      return TextureFormat::Invalid;
  }
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

Texture *GLTexturePool::acquire_texture(int2 extent,
                                        TextureFormat format,
                                        eGPUTextureUsage usage,
                                        const char *name)
{
  /* Determine format of compatible underlying texture. If there is no
   * compatible format to alias upon, we simply require an exact match
   * for the underlying texture. */
  TextureFormat compatible_format = get_compatible_texture_format(format);
  if (compatible_format == TextureFormat::Invalid) {
    compatible_format = format;
  }

  /* Search for the first compatible existing texture. */
  int64_t match_index = -1;
  for (uint64_t i : pool_.index_range()) {
    const auto &handle = pool_[i];
    if (handle.texture->format_get() != compatible_format) {
      continue;
    }
    if (int2(handle.texture->w_, handle.texture->h_) != extent) {
      continue;
    }
    match_index = i;
    break;
  }

  /* Return value. */
  TextureHandle texture_handle;

  /* Acquire the compatible texture, or create a new one as a last resort. */
  if (match_index != -1) {
    texture_handle.texture_allocation = pool_[match_index].texture;
    pool_.remove_and_reorder(match_index);
  }
  else {
    /* Debug label attached to allocated texture object. */
    std::string texture_name_str;
    // if (G.debug & G_DEBUG_GPU) {
    texture_name_str = fmt::format("TexFromPool_{}", pool_.size());
    // }

    eGPUTextureUsage usage_flag = usage | GPU_TEXTURE_USAGE_FORMAT_VIEW;
    texture_handle.texture_allocation = unwrap(GPU_texture_create_2d(
        texture_name_str.c_str(), extent.x, extent.y, 1, compatible_format, usage_flag, nullptr));
  }

  /* Debug label attached to view texture object. */
  std::string view_name_str;
  // if (G.debug & G_DEBUG_GPU) {
  view_name_str = name ? name : texture_handle.texture_allocation->name_;
  // }

  /* Assemble texture view and add to handle. Note, glTextureView with identical formats is
   * allowed, even if the formats are not listed for aliasing in the Internal Formats table. */
  gpu::Texture *view = GPU_texture_create_view(
      view_name_str.c_str(), texture_handle.texture_allocation, format, 0, 1, 0, 1, false, false);
  texture_handle.texture = unwrap(view);

  /* if (format != compatible_format) {
    std::printf("Aliasing: %s (%d) -> %s (%d)\n",
                GPU_texture_format_name(texture_handle.texture_allocation->format_get()),
                to_bytesize(texture_handle.texture_allocation->format_get()),
                GPU_texture_format_name(texture_handle.texture->format_get()),
                to_bytesize(texture_handle.texture->format_get()));
  } */

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

  /* Destroy view and handle, if a view was created. */
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
