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

class VKTexturePool : public TexturePool {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  /* All performed allocations are multiplied by this factor as a temporary metric. */
  static constexpr VkDeviceSize allocation_size = 67108864;

  struct Segment {
    VkDeviceSize offset;
    VkDeviceSize size;
  };

  /* Struct to manage a memory allocation and its unused segments. */
  struct AllocationHandle {
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocation_info = {};

    /* Counter to track the number of unused cycles before deallocation in `pool_`. */
    int unused_cycles_count = 0;

    /* Linked list of unused segments of the allocation. */
    std::list<Segment> segments;

    /* Allocate/deallocate the handle internals. */
    bool alloc(VkMemoryRequirements memory_requirements);
    void free();

    /* Extract a segment of the allocation, if compatible. */
    std::optional<Segment> acquire(VkMemoryRequirements memory_requirements);

    /* Return a segment to the allocation for reuse. */
    void release(Segment region);

    /* Check if the allocation is entirely unused. */
    bool is_unused() const
    {
      return !segments.empty() && segments.front().size == allocation_info.size;
    }

    /* We use the pointer as hash/comparator, as a VmaAllocation is unique.
     * This means we can find the handle without knowing other internals. */
    uint64_t hash() const
    {
      return get_default_hash(allocation);
    }

    bool operator==(const AllocationHandle &o) const
    {
      return allocation == o.allocation;
    }
  };

  /* Struct to store an acquired texture and its backing allocation. */
  struct TextureHandle {
    VKTexture *texture = nullptr;
    AllocationHandle allocation_handle = {};
    Segment segment = {};

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
  Set<AllocationHandle> allocations_;
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

#ifndef NDEBUG
  void debug_usage_log() const;
#endif
};

}  // namespace blender::gpu
