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

/* TODO(not_mark): implement destructor. */
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

  /* Create new output object, and fill known texture properties.
   * Internal counter is set to 1 on acquire. */
  char name[16] = "TexFromPool";
  if (G.debug & G_DEBUG_GPU) {
    int texture_id = acquired_.size();
    SNPRINTF(name, "TexFromPool_%d", texture_id);
  }
  VKTexture *vk_tex = new VKTexture(name);
  vk_tex->w_ = extent.x;
  vk_tex->h_ = extent.y;
  vk_tex->d_ = 0;
  vk_tex->format_ = format;
  vk_tex->format_flag_ = to_format_flag(format);
  vk_tex->type_ = GPU_TEXTURE_2D;
  vk_tex->gpu_image_usage_flags_ = usage;

  /* R16G16F16 formats are typically not supported (<1%). */
  vk_tex->device_format_ = format;
  if (vk_tex->device_format_ == TextureFormat::SFLOAT_16_16_16) {
    vk_tex->device_format_ = TextureFormat::SFLOAT_16_16_16_16;
  }
  if (vk_tex->device_format_ == TextureFormat::SFLOAT_32_32_32) {
    vk_tex->device_format_ = TextureFormat::SFLOAT_32_32_32_32;
  }

  /* Mirrors behavior in gpu::Texture::init_2d(...). */
  if ((vk_tex->format_flag_ & (GPU_FORMAT_DEPTH_STENCIL | GPU_FORMAT_INTEGER)) == 0) {
    vk_tex->sampler_state.filtering = GPU_SAMPLER_FILTERING_LINEAR;
  }

  /* Fill a VkImage info object. */
  VkExtent3D image_extent;
  image_extent.width = static_cast<uint32_t>(extent.x);
  image_extent.height = static_cast<uint32_t>(extent.y);
  image_extent.depth = 1u;
  VkImageCreateInfo image_create_info = {};
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.flags = to_vk_image_create(GPU_TEXTURE_2D, to_format_flag(format), usage);
  image_create_info.usage = to_vk_image_usage(usage, to_format_flag(format), false);
  image_create_info.format = to_vk_format(format);
  image_create_info.extent = image_extent;
  image_create_info.arrayLayers = 1;
  image_create_info.mipLevels = 1;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.queueFamilyIndexCount = 1;
  const uint32_t queue_family_indices[1] = {device.queue_family_get()};
  image_create_info.pQueueFamilyIndices = queue_family_indices;

  /* Initialize VkImage object. */
  VkResult result = vkCreateImage(
      device.vk_handle(), &image_create_info, nullptr, &(vk_tex->vk_image_));
  BLI_assert_msg(result == VK_SUCCESS,
                 "Failed to create VkImage in VKTexturePool::acquire_texture");

  /* Query the requirements for this specific image */
  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(device.vk_handle(), vk_tex->vk_image_, &memory_requirements);

  /* Search for an existing compatible allocation. */
  int64_t match_index = -1;
  for (uint64_t i : free_.index_range()) {
    /* TODO(not_mark): what about alignment :( ? */
    const auto &handle = free_[i];
    if (handle.allocation_info.size >= memory_requirements.size) {
      /* `memory_requirements.memoryTypeBits` has bits set for every type of supported memory;
       * only one needs to match for the allocation to be compatible to the image. */
      if (bool(handle.allocation_info.memoryType & memory_requirements.memoryTypeBits)) {
        match_index = i;
        break;
      }
    }
  }

  /* Set allocation data, and bind image to allocation. */
  if (match_index != -1) {
    /* If a compatible allocation was found, copy handles and acquire it. */
    const auto &handle = free_[match_index];
    vk_tex->allocation_ = handle.allocation;
    vk_tex->allocation_info_ = handle.allocation_info;
    free_.remove_and_reorder(match_index);
  }
  else {
    /* Otherwise, allocate new compatible memory. */
    VmaAllocationCreateInfo allocation_create_info = {};
    allocation_create_info.priority = 0.5f;  // memory_priority(usage); /* TODO export function */
    allocation_create_info.memoryTypeBits = memory_requirements.memoryTypeBits;
    allocation_create_info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    vmaAllocateMemory(device.mem_allocator_get(),
                      &memory_requirements,
                      &allocation_create_info,
                      &(vk_tex->allocation_),
                      &(vk_tex->allocation_info_));
  }
  vmaBindImageMemory(device.mem_allocator_get(), vk_tex->allocation_, vk_tex->vk_image_);

  debug::object_label(vk_tex->vk_image_, vk_tex->name_);
  device.resources.add_image(vk_tex->vk_image_, false, vk_tex->name_);

  acquired_.add({vk_tex, 1}); /* Internal counter set to 1 on acquire. */
  return vk_tex;
}

void VKTexturePool::release_texture(Texture *tex)
{
  BLI_assert_msg(acquired_.remove({tex, 1}),
                 "Unacquired texture passed to TexturePool::release_texture()");

  auto vk_tex = static_cast<VKTexture *>(tex);

  /* The image is forwarded for discard, but the allocation is retained. There
   * is no need to explicitly unbind the image from the allocation in VMA. */
  VKDiscardPool::discard_pool_get().discard_image(vk_tex->vk_image_, VK_NULL_HANDLE);

  /* Gather internal allocation data and copy to `free_`. */
  AllocationHandle allocation_handle;
  allocation_handle.allocation = vk_tex->allocation_;
  allocation_handle.allocation_info = vk_tex->allocation_info_;
  allocation_handle.usage = vk_tex->gpu_image_usage_flags_;
  allocation_handle.unused_cycles_counter = 0;
  free_.append(allocation_handle);

  /* Set internals to VK_NULL_HANDLE to avoid the VKTexture internal destructor. */
  vk_tex->vk_image_ = VK_NULL_HANDLE;
  vk_tex->allocation_ = VK_NULL_HANDLE;
  delete vk_tex;
}

void VKTexturePool::reset(bool force_free)
{
  VKDevice &device = VKBackend::get().device;

#ifndef NDEBUG
  /* Iterate acquired textures, and ensure the internal counter equals 0; otherwise
   * this indicates a missing `::retain()` or `::release()`. */
  for (const TextureHandle &tex : acquired_) {
    BLI_assert_msg(tex.counter == 0,
                   "Missing texture release/retain. Likely TextureFromPool::release(), "
                   "TextureFromPool::retain() or TexturePool::release_texture().");
  }
#endif

  /* Reverse iterate unused allocations, to make sure we only reorder known good handles. */
  for (int i = free_.size() - 1; i >= 0; i--) {
    AllocationHandle &handle = free_[i];
    if (handle.unused_cycles_counter >= max_unused_cycles_ || force_free) {
      /* FIXME(not_mark): this needs to go to discard pool, but for that it needs to be tracked. */
      vmaFreeMemory(device.mem_allocator_get(), handle.allocation);
      // VKDiscardPool::discard_pool_get().discard_image(VK_NULL_HANDLE, handle.allocation);
      free_.remove_and_reorder(i);
    }
    else {
      handle.unused_cycles_counter++;
    }
  }

  std::printf("VKTexturePool: free_=%d, acquired_=%d\n", free_.size(), acquired_.size());
}

void VKTexturePool::offset_texture_counter(Texture *tex, int offset)
{
  int counter = acquired_.lookup_key({tex, 0}).counter;
  acquired_.add_overwrite({tex, counter + offset});
}

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
