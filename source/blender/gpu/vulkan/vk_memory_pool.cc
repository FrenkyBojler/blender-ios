/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_memory_pool.hh"

#include "vk_device.hh"

namespace blender::gpu {

void VKMemoryPools::init(VKDevice &device)
{
  if (device.extensions_get().external_memory) {
    init_external_memory_image(device);
    init_external_memory_pixel_buffer(device);
  }
}

void VKMemoryPools::init_external_memory_image(VKDevice &device)
{
  VkExternalMemoryImageCreateInfo external_image_create_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = nullptr,
      .handleTypes = vk_external_memory_handle_type()};
  VkImageCreateInfo image_create_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                         .pNext = &external_image_create_info,
                                         .flags = 0,
                                         .imageType = VK_IMAGE_TYPE_2D,
                                         .format = VK_FORMAT_R8G8B8A8_UNORM,
                                         .extent = {1024, 1024, 1},
                                         .mipLevels = 1,
                                         .arrayLayers = 1,
                                         .samples = VK_SAMPLE_COUNT_1_BIT,
                                         .tiling = VK_IMAGE_TILING_OPTIMAL,
                                         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                         .queueFamilyIndexCount = 0,
                                         .pQueueFamilyIndices = nullptr,
                                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  VmaAllocationCreateInfo allocation_create_info = {
      .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
      .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
  uint32_t memory_type_index;
  vmaFindMemoryTypeIndexForImageInfo(
      device.mem_allocator_get(), &image_create_info, &allocation_create_info, &memory_type_index);

  external_memory_image.info.handleTypes = vk_external_memory_handle_type();
  VmaPoolCreateInfo pool_create_info = {.memoryTypeIndex = memory_type_index,
                                        .priority = 1.0f,
                                        .pMemoryAllocateNext = &external_memory_image.info};
  vmaCreatePool(device.mem_allocator_get(), &pool_create_info, &external_memory_image.pool);
}

void VKMemoryPools::init_external_memory_pixel_buffer(VKDevice &device)
{
  VkExternalMemoryBufferCreateInfo external_buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
      .pNext = nullptr,
      .handleTypes = vk_external_memory_handle_type()};
  VkBufferCreateInfo buffer_create_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                           .pNext = &external_buffer_create_info,
                                           .flags = 0,
                                           .size = 1024,
                                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                           .queueFamilyIndexCount = 0,
                                           .pQueueFamilyIndices = nullptr};
  VmaAllocationCreateInfo allocation_create_info = {
      .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
      .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
  uint32_t memory_type_index;
  vmaFindMemoryTypeIndexForBufferInfo(device.mem_allocator_get(),
                                      &buffer_create_info,
                                      &allocation_create_info,
                                      &memory_type_index);

  external_memory_pixel_buffer.info.handleTypes = vk_external_memory_handle_type();
  VmaPoolCreateInfo pool_create_info = {.memoryTypeIndex = memory_type_index,
                                        .priority = 1.0f,
                                        .pMemoryAllocateNext = &external_memory_pixel_buffer.info};
  vmaCreatePool(device.mem_allocator_get(), &pool_create_info, &external_memory_pixel_buffer.pool);
}

void VKMemoryPools::deinit(VKDevice &device)
{
  external_memory_image.deinit(device);
  external_memory_pixel_buffer.deinit(device);
}

void VKMemoryPool::deinit(VKDevice &device)
{
  vmaDestroyPool(device.mem_allocator_get(), pool);
}

}  // namespace blender::gpu
