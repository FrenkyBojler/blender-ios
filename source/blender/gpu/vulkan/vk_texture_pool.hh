/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_texture_pool_private.hh"
#include "vk_texture.hh"
#include <list>
#include <optional>

namespace blender::gpu {

class VKTexturePool final : public TexturePool {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  struct AllocationHandle {
    VmaAllocation allocation;
    VmaAllocationInfo allocation_info;
    int unused_cycles_counter;

   public:
    void init(VkMemoryRequirements memory_requirements);
    void free();
  };

  struct TextureHandle {
    Texture *texture;
    int counter;

   public:
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

  Vector<AllocationHandle> free_;
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
