/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_texture_pool_private.hh"

namespace blender::gpu {

class VKTexturePool : public TexturePool {
  struct Page {
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocation_info = {};

    VkFormat format;
    eGPUTextureUsage usage;

    uint unused_cycles = 0;
  };

  struct Block {
    uint heap_index;
    size_t offset, size;

    /* heap_index; */
    /* offset; */
    /* size; */
    /* formats; */
  };

  struct Allocation {
    VkImage image;

    /* heap_index; */
    /* offset; */
    /* size; */
    /* format; */
    /* texture_handle; */
  };

  Vector<Page> pages_;
  Vector<Block> free_;
  Vector<Allocation> acquired_transient_;
  Vector<Allocation> acquired_persistent_;

  void allocate_compatible_page(VkImage image);
  uint get_compatible_page_index(VkImage image);

 public:
  VKTexturePool();
  ~VKTexturePool();

  Texture *acquire_texture(int2 extent,
                           TextureFormat format,
                           eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL) final;

  void release_texture(Texture *tmp_tex) final;

  void reset(bool force_free = false) final;

  void offset_texture_counter(Texture *tex, int offset) final;
};

}  // namespace blender::gpu
