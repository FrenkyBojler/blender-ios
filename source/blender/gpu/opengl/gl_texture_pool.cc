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
    return gpu::TextureFormat::Invalid;
  }
  if (bool(format_flag & GPU_FORMAT_COMPRESSED)) {
    return gpu::TextureFormat::Invalid;
  }

  switch (to_bytesize(format)) {
    case 16:
      return gpu::TextureFormat::UINT_32_32_32_32;
    case 8:
      return gpu::TextureFormat::UINT_32_32;
    case 4:
      return gpu::TextureFormat::UINT_32;
    case 2:
      return gpu::TextureFormat::UINT_16;
    case 1:
      return gpu::TextureFormat::UINT_8;
    default:
      return gpu::TextureFormat::Invalid;
  }
}

static bool are_formats_compatible(const TextureFormat format_a, const TextureFormat format_b)
{
  GPUTextureFormatFlag flag_a = to_format_flag(format_a);
  GPUTextureFormatFlag flag_b = to_format_flag(format_b);
  bool is_depth_or_stencil = (flag_a & GPU_FORMAT_DEPTH_STENCIL) != 0 ||
                             (flag_b & GPU_FORMAT_DEPTH_STENCIL) != 0;
  bool is_compressed = (flag_a & GPU_FORMAT_COMPRESSED) != 0 ||
                       (flag_b & GPU_FORMAT_COMPRESSED) != 0;

  if (GPU_type_matches(GPU_DEVICE_INTEL, GPU_OS_ANY, GPU_DRIVER_ANY)) {
    bool is_bad_format_match =
        /* RG16F and R11F_G11F_B10F appear to be incompatible. glCLear* operations fail. */
        (format_a == TextureFormat::UFLOAT_11_11_10 && format_b == TextureFormat::SFLOAT_16_16) ||
        /* sRGB and non-sRGB appear to be incompatible */
        (flag_a & GPU_FORMAT_SRGB) == (flag_b & GPU_FORMAT_SRGB);
    if (is_bad_format_match) {
      return false;
    }
  }

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

Texture *GLTexturePool::acquire_texture(int2 extent,
                                        TextureFormat format,
                                        eGPUTextureUsage usage,
                                        const char *name)
{
  /* Search for the first compatible existing texture. */
  int64_t match_index = -1;
  for (uint64_t i : pool_.index_range()) {
    const auto &handle = pool_[i];
    if (int2(handle.texture->w_, handle.texture->h_) != extent) {
      continue;
    }

    TextureFormat compatible_format = get_compatible_texture_format(format);
    if (compatible_format == TextureFormat::Invalid) {
      compatible_format = format;
    }
    if (compatible_format != handle.texture->format_get()) {
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
    std::string texture_name_str;
#ifndef NDEBUG
    texture_name_str = fmt::format("TexFromPool_{}", pool_.size());
#endif
    eGPUTextureUsage usage_flag = usage | GPU_TEXTURE_USAGE_FORMAT_VIEW;
    texture_handle.texture_allocation = unwrap(GPU_texture_create_2d(
        texture_name_str.c_str(), extent.x, extent.y, 1, format, usage_flag, nullptr));
  }

  /* Assemble texture view and add to handle. Note, glTextureView with identical formats is
   * allowed, even if the formats are not listed for aliasing in the Internal Formats table. */
  std::string view_name_str;
#ifndef NDEBUG
  view_name_str = name ? name : texture_handle.texture_allocation->name_;
#endif
  gpu::Texture *view = GPU_texture_create_view(
      view_name_str.c_str(), texture_handle.texture_allocation, format, 0, 1, 0, 1, false, false);
  texture_handle.texture = unwrap(view);

  acquired_.add(texture_handle);
  return view;
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
