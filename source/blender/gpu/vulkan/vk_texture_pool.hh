/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_texture_pool_private.hh"
#include <list>

namespace blender::gpu {

/*
- Create a PageHandle
- Which contains an allocation and such
- And also contains a list of Allocations
  - Which are offsets, sizes, properly aligned if necessary.
-


*/

class VKTexturePool : public TexturePool {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;
  struct PageRegion {
    VkDeviceSize offset;
    VkDeviceSize size;
  };

  /* Struct to store a memory allocation, and available regions of the allocation. */
  struct PageHandle {
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocation_info = {};

    /* List of unused regions of the allocation. */
    std::list<PageRegion> regions;

    /* Counter to track the number of unused cycles before deallocation in `pool_`. */
    int unused_cycles_count = 0;

    /* Allocate/deallocate the handle internals. */
    bool alloc(VkMemoryRequirements memory_requirements);
    void free();

    /* Extract the first available region of the allocation, if it is compatible */
    std::optional<PageRegion> acquire_region(VkMemoryRequirements memory_requirements);

    /* Return a region to the allocation as available for reuse. */
    void release_region(PageRegion region);

    bool is_unused() const
    {
      if (regions.size() == 1u) {
        return regions.front().size == allocation_info.size;
      }
      return false;
    }

    /* We use the pointer as hash/comparator, as a VmaAllocation is unique.
     * This means we can find the handle without knowing other internals. */
    uint64_t hash() const
    {
      return get_default_hash(allocation);
    }

    bool operator==(const PageHandle &o) const
    {
      return allocation == o.allocation;
    }
  };

  /* Struct to store an acquired texture and its backing allocation. */
  struct TextureHandle {
    VKTexture *texture = nullptr;
    PageHandle page_handle = {};
    PageRegion page_region = {};

    /* Counter to track texture acquire/retain mismatches in `acquire_`.  */
    int users_count = 1;

    /* Create/destroy the VKTexture+VkImage backing the internal pointer. */
    bool alloc(int2 extent, TextureFormat format, eGPUTextureUsage usage, const char *name);
    void free();

    /* We use the pointer as hash/comparator, as a TextureHandle cannot be acquired twice.
     * This means we can find the handle without knowing the internal counter. */
    uint64_t hash() const
    {
      return get_default_hash(texture);
    }

    bool operator==(const TextureHandle &o) const
    {
      return texture == o.texture;
    }
  };

  /* Store of allocated blocks, potentially partially in use. */
  Set<PageHandle> pages_;
  /* Store of acquired textures. */
  Set<TextureHandle> acquired_;

 public:
  ~VKTexturePool();

  Texture *acquire_texture(int2 extent,
                           TextureFormat format,
                           eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL,
                           const char *name = nullptr) override;

  void release_texture(Texture *tex) override;

  void reset(bool force_free = false) override;

  void offset_users_count(Texture *tex, int offset) override;
};

}  // namespace blender::gpu
