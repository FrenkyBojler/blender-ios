/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_capabilities.hh"

/* vk_common needs to be included first to ensure win32 vulkan API is fully initialized, before
 * working with it. */
#include "vk_common.hh"
#include "vk_texture_pool.hh"

namespace blender::gpu {
  VKTexturePool::VKTexturePool()
  {
    /* ... */
  }

  VKTexturePool::~VKTexturePool()
  {
    /* ... */
  }

  Texture *VKTexturePool::acquire_texture(int width,
                           int height,
                           TextureFormat format,
                           eGPUTextureUsage usage,
                           eGPUTextureLifetime lifetime)
  {
    /* ... */
    return;
  }

  void VKTexturePool::release_texture(Texture *tmp_tex)
  {
    /* ... */
    return;
  }

  void VKTexturePool::make_texture_persistent(Texture *tex)
  {
    /* ... */
    return;
  }
  void VKTexturePool::make_texture_transient(Texture *tex)
  {
    /* ... */
    return;
  }

  bool VKTexturePool::is_texture_persistent(Texture *tex) const
  {
    /* ... */
    return;
  }
  bool VKTexturePool::is_texture_transient(Texture *tex) const
  {
    /* ... */
    return;
  }

  void VKTexturePool::reset(bool force_free = false)
  {
    /* ... */
    return;
  }
} // namespace blender::gpu