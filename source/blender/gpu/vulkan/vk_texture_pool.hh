/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_texture_pool_private.hh"

namespace blender::gpu {

class VKTexturePool final : public TexturePool {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  /* Struct to store unused allocations. The internal counter increments on every
   * `::reset()`, and the allocation is deallocated when it reaches `max_unused_cycles_`. */
  struct AllocationHandle {
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocation_info = {};
    int counter = 0;

    /* Allocate/deallocate the handle internals. */
    void init(VkMemoryRequirements memory_requirements);
    void free();
  };

  /* Struct to store acquired textures and the backing allocation. The internal counter is set to 1
   * on `::acquire()` and decrements on `::release()/::retain()`, and must be 0 on `::reset()`. */
  struct TextureHandle {
    VKTexture *texture = nullptr;
    AllocationHandle allocation_handle = {};
    int counter = 1;

    /* Create or destroy the VKTexture+VkImage backing the internal pointer. */
    void init(int2 extent, TextureFormat format, eGPUTextureUsage usage, const char *name);
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

  Vector<AllocationHandle> pool_;
  Set<TextureHandle> acquired_;

 public:
  ~VKTexturePool();
  Texture *acquire_texture(int2 extent,
                           TextureFormat format,
                           eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL) final;
  void release_texture(Texture *tex) final;
  void reset(bool force_free = false) final;
  void offset_texture_counter(Texture *tex, int offset) final;
};

}  // namespace blender::gpu
