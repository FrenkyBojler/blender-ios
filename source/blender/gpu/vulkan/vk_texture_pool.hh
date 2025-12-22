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

struct VKTexturePool : public TexturePool {
  /* Defer deallocation enough cycles to avoid interleaved calls to different viewport render
   * functions (selection / display) causing constant allocation / deallocation (See #113024). */
  static constexpr int max_unused_cycles_ = 8;

  struct AllocationHandle {
    VmaAllocation allocation;
    VmaAllocationInfo allocation_info;
    eGPUTextureUsage usage;
    int unused_cycles_counter;
  };

  struct TextureHandle {
    Texture *texture;
    int counter;

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

  // /* Forward declaration. */
  // struct PageHandle;
  // struct BlockHandle;
  // struct TextureHandle;

  // // struct Page {
  // //   VmaAllocation allocation = VK_NULL_HANDLE;
  // //   VmaAllocationInfo allocation_info = {};

  // //   VkFormat format;
  // //   eGPUTextureUsage usage;

  // //   uint unused_cycles = 0;
  // // };

  // // struct Block {
  // //   uint heap_index;
  // //   size_t offset, size;

  // //   /* heap_index; */
  // //   /* offset; */
  // //   /* size; */
  // //   /* formats; */
  // // };

  // // struct Allocation {
  // //   VkImage image;

  // //   /* heap_index; */
  // //   /* offset; */
  // //   /* size; */
  // //   /* format; */
  // //   /* texture_handle; */
  // // };

  // // Vector<Page> pages_;
  // // Vector<Block> free_;
  // // Vector<Allocation> acquired_transient_;-
  // // Vector<Allocation> acquired_persistent_;

  // // void allocate_compatible_page(VkImage image);
  // // uint get_compatible_page_index(VkImage image);

  // /* A page is an allocation of texture memory of a specific format and type,
  //  * that can be split into one or more blocks used by acquired textures. */
  // struct PageHandle {
  //   VmaAllocation allocation = VK_NULL_HANDLE;
  //   VmaAllocationInfo allocation_info = {};
  //   size_t extent;

  //   /* While `::is_unused()` holds, we wait several cycles before deallocation. */
  //   int unused_counter;

  //   /* Linked lists are used as insert/remove/split/merge of blocks is common.  */
  //   std::list<BlockHandle> free_;
  //   std::list<BlockHandle> used_;

  //  public:
  //   BlockHandle *acquire()
  //   {
  //     /* TODO(not_mark): implement. */
  //     return nullptr;
  //   }

  //   void release(BlockHandle *block)
  //   {
  //     /* TODO(not_mark): implement. */
  //   }

  //   bool is_unused() const
  //   {
  //     return used_.is_empty();
  //   }
  // };

  // /* A block is part of a page, either in use or not, that can be split
  //  * in smaller blocks, or merged with neighbors. */
  // struct BlockHandle {
  //   PageHandle &page;
  //   size_t offset;
  //   size_t extent;
  // };

  // struct TextureHandle {
  //   BlockHandle &block;
  //   Texture *texture;

  //   /* We track `::acquire()/::retain()` offsets to ensure proper usage. */
  //   int retained_counter;

  //   /* We use the pointer as hash/comparator, as a texture cannot be acquired twice.
  //    * This means we can find the handle without knowing any other internals.  */
  //   uint64_t hash() const
  //   {
  //     return get_default_hash(texture);
  //   }

  //   bool operator==(const TextureHandle &o) const
  //   {
  //     return texture == o.texture;
  //   }
  // };

  // /* Use std::list so pointers are not invalidated on resize. */
  // std::list<PageHandle> pages_;
  // Set<TextureHandle> acquired_;

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
