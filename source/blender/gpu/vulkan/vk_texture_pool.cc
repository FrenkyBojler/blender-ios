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

#include "vk_backend.hh"
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

void VKTexturePool::allocate_compatible_page(VkImage image)
{
  VKDevice &device = VKBackend::get().device;

  /* Query `memoryTypeBits` for this specific image */
  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(device.vk_handle(), image, &memory_requirements);

  VmaAllocationCreateInfo allocation_create_info = {};
  allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  allocation_create_info.memoryTypeBits = memory_requirements.memoryTypeBits;
  /* allocation_create_info.priority =  */ /* TODO(not_mark): get from vk_texture.cc */

  /* ... */
}

uint VKTexturePool::get_compatible_page_index(VkImage image)
{
  VKDevice &device = VKBackend::get().device;

  /* Query the requirements for this specific image */
  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(device.vk_handle(), image, &memory_requirements);

  return 0u;
}

Texture *VKTexturePool::acquire_texture(int width,
                                        int height,
                                        TextureFormat format,
                                        eGPUTextureUsage usage,
                                        eGPUTextureLifetime lifetime)
{
  VKDevice &device = VKBackend::get().device;

  /* Fill VkImage creation objects. */
  /* FIXME(not_mark): hardcoded to 2D for now. */
  VkExtent3D extent;
  extent.width = width;
  extent.height = height;
  extent.depth = 1;
  VkImageCreateInfo image_create_info = {};
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.flags = to_vk_image_create(GPU_TEXTURE_2D, to_format_flag(format), usage);
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.extent = extent;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.format = to_vk_format(format);
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_create_info.usage = to_vk_image_usage(usage, to_format_flag(format));
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;

  /* Query specific VkImage device support. */
  /* TODO(not_mark): implement debug feature to halt on missing device support. */

  /* Create VkImage object without bound memory. */
  VkImage image;
  VkResult result = vkCreateImage(device.vk_handle(), &image_create_info, nullptr, &image);
  BLI_assert_msg(result == VK_SUCCESS, "Failed to create image in VKTexturePool::acquire_texture");

  /* Find available block in compatible page. */

  /* Split block from page. */

  /* Bind block to VkImage object. */

  // image_create_info.flags = to_vk_image_
  // vkCreateImage(device.vk_handle(), 1)

  return nullptr;
}

void VKTexturePool::release_texture(Texture *tmp_tex)
{
  /* Unbind block from VkImage object, or destroy VkImage object. */

  /* Return block to page. */

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
  return true;
}
bool VKTexturePool::is_texture_transient(Texture *tex) const
{
  /* ... */
  return true;
}

void VKTexturePool::reset(bool force_free)
{
  /* ... */
  return;
}
}  // namespace blender::gpu
