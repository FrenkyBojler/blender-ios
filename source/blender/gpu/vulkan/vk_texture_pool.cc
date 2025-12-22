/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_capabilities.hh"

/* vk_common needs to be included first to ensure win32 vulkan API is fully initialized, before
 * working with it. */
#include "vk_backend.hh"
#include "vk_common.hh"
#include "vk_texture_pool.hh"

namespace blender::gpu {

// std::pair<VKTexturePool::BlockHandle, VKTexturePool::BlockHandle> split_block(
//     VKTexturePool::BlockHandle block, size_t split_offset)
// {
//   BLI_assert(split_offset < block.extent);
//   VKTexturePool::BlockHandle left = {block.page, block.offset, split_offset};
//   VKTexturePool::BlockHandle right = {
//       block.page, block.offset + split_offset, block.extent - split_offset};
//   return {left, right};
// }

// VKTexturePool::BlockHandle merge_blocks(VKTexturePool::BlockHandle left_block,
//                                         VKTexturePool::BlockHandle right_block)
// {
//   BLI_assert(left_block.page == right_block.page);
//   BLI_assert(left_block.offset + left_block.extent == right_block.offset - 1);
//   return {left_block.page, left_block.offset, left_block.extent + right_block.extent};
// }

VKTexturePool::~VKTexturePool()
{
  for (auto &handle : acquired_) {
    /* ... */
  }
  for (auto &handle : free_) {
    /* ... */
  }
}

Texture *VKTexturePool::acquire_texture(int2 extent, TextureFormat format, eGPUTextureUsage usage)
{
  VKDevice &device = VKBackend::get().device;
  
  /* Fill a VkImage create object. */
  VkImageCreateInfo image_create_info = {};
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.flags = to_vk_image_create(GPU_TEXTURE_2D, to_format_flag(format), usage);
  image_create_info.usage = to_vk_image_usage(usage, to_format_flag(format));
  image_create_info.format = to_vk_format(format);
  image_create_info.extent = { extent.x, extent.y, 1u };
  image_create_info.arrayLayers = 1;
  image_create_info.mipLevels = 1;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  /* Create new output object, and fill known texture properties. 
   * Internal counter is set to 1 on acquire. */
  TextureHandle texture_handle = { new VKTexture(), 1 };
  texture_handle.texture->device_format_ = format;
  /* FIXME(not_mark): did I get all of these? */

  /* Handles to texture internals, uninitialized */
  VkImage &image = texture_handle.texture->vk_image_;
  VmaAllocation &allocation = texture_handle.texture->allocation_;
  VmaAllocation &allocation_info = texture_handle.texture->allocation_info_;

  /* Initialize VkImage object. */
  VkResult result = vkCreateImage(device.vk_handle(), &image_create_info, nullptr, &image);
  BLI_assert_msg(result == VK_SUCCESS, "Failed to create image in VKTexturePool::acquire_texture");

  /* Query the requirements for this specific image */
  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(device.vk_handle(), image, &memory_requirements);

  /* Query specific VkImage device support. */
  // ...

  /* Search for an existing compatible allocation. */
  int64_t match_index = -1;
  for (uint64_t i : free_.index_range()) {
    /* TODO(not_mark): what about alignment :( ? */
    const auto &handle = free_[i];
    if (handle.allocation_info.size >= memory_requirements.size) {
      /* `memory_requirements.memoryTypeBits` has bits set for every type of supported memory; 
       * only one needs to match for the allocation to be compatible. */
      if (bool(handle.allocation_info.memoryType & memory_requirements.memoryTypeBits)) {
        match_index = i;
        break;
      }
    }
  }
  
  /* Set allocation data, and bind image to allocation. */
  if (match_index != -1) {
    /* If a compatible allocation was found, copy handles and acquire it. */
    const auto &handle = free_[i];
    allocation = handle.allocation;
    allocation_info = handle.allocation_info;
    free_.remove_and_reorder(match_index);
  }
  else {
    /* Otherwise, allocate new compatible memory. */
    VmaAllocationCreateInfo allocation_create_info = {};
    allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocation_create_info.memoryTypeBits = memory_requirements.memoryTypeBits;
    allocation_create_info.priority = 0.5f; // memory_priority(usage); /* TODO export function */
    vmaAllocateMemoryForImage(device.mem_allocator_get(),
                              image,
                            &allocation_create_info,
                            &allocation,
                            &allocation_info);
  }
  vmaBindImageMemory(device.mem_allocator_get(), allocation, image);

  acquired_.append(texture_handle);
  return texture_handle.texture;
}

void VKTexturePool::release_texture(Texture *tex)
{

}

void VKTexturePool::reset(bool force_free = false) {}

void VKTexturePool::offset_texture_counter(Texture *tex, int offset) {}

// void VKTexturePool::allocate_compatible_page(VkImage image)
// {
//   VKDevice &device = VKBackend::get().device;

//   /* Query `memoryTypeBits` for this specific image */
//   VkMemoryRequirements memory_requirements;
//   vkGetImageMemoryRequirements(device.vk_handle(), image, &memory_requirements);

//   VmaAllocationCreateInfo allocation_create_info = {};
//   allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
//   allocation_create_info.memoryTypeBits = memory_requirements.memoryTypeBits;
//   /* allocation_create_info.priority =  */ /* TODO(not_mark): get from vk_texture.cc */

//   /* ... */
// }

// uint VKTexturePool::get_compatible_page_index(VkImage image)
// {
//   VKDevice &device = VKBackend::get().device;

//   /* Query the requirements for this specific image */
//   VkMemoryRequirements memory_requirements;
//   vkGetImageMemoryRequirements(device.vk_handle(), image, &memory_requirements);

//   return 0u;
// }

// Texture *VKTexturePool::acquire_texture(int width,
//                                         int height,
//                                         TextureFormat format,
//                                         eGPUTextureUsage usage,
//                                         eGPUTextureLifetime lifetime)
// {
//   VKDevice &device = VKBackend::get().device;

//   /* Fill VkImage creation objects. */
//   /* FIXME(not_mark): hardcoded to 2D for now. */
//   VkExtent3D extent;
//   extent.width = width;
//   extent.height = height;
//   extent.depth = 1;
//   VkImageCreateInfo image_create_info = {};
//   image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
//   image_create_info.flags = to_vk_image_create(GPU_TEXTURE_2D, to_format_flag(format), usage);
//   image_create_info.imageType = VK_IMAGE_TYPE_2D;
//   image_create_info.extent = extent;
//   image_create_info.mipLevels = 1;
//   image_create_info.arrayLayers = 1;
//   image_create_info.format = to_vk_format(format);
//   image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
//   image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
//   image_create_info.usage = to_vk_image_usage(usage, to_format_flag(format));
//   image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;

//   /* Query specific VkImage device support. */
//   /* TODO(not_mark): implement debug feature to halt on missing device support. */

//   /* Create VkImage object without bound memory. */
//   VkImage image;
//   VkResult result = vkCreateImage(device.vk_handle(), &image_create_info, nullptr, &image);
//   BLI_assert_msg(result == VK_SUCCESS, "Failed to create image in
//   VKTexturePool::acquire_texture");

//   /* Find available block in compatible page. */

//   /* Split block from page. */

//   /* Bind block to VkImage object. */

//   // image_create_info.flags = to_vk_image_
//   // vkCreateImage(device.vk_handle(), 1)

//   return nullptr;
// }
}  // namespace blender::gpu
