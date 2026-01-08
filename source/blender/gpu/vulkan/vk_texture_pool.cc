/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_capabilities.hh"

#include "vk_backend.hh"
#include "vk_texture.hh"
#include "vk_texture_pool.hh"

#include "fmt/format.h"

namespace blender::gpu {

static VkDeviceSize align_size(VkDeviceSize offset, VkDeviceSize alignment)
{
  return (offset - 1u + alignment) & -alignment;
}

std::optional<VKTexturePool::PageRegion> VKTexturePool::PageHandle::acquire_region(
    VkMemoryRequirements memory_requirements)
{
  if (!bool(memory_requirements.memoryTypeBits & allocation_info.memoryType)) {
    return {};
  }

  /* Modify size up to alignment requirements. We always extend at the end 
   * so a segment after our acquisition remains aligned. */
  memory_requirements.size = align_size(memory_requirements.size, memory_requirements.alignment);

  /* Find the smallest region of compatible size. */
  auto it = regions.end();
  for (auto iter = regions.begin(); iter != regions.end(); ++iter) {
    if (iter->size < memory_requirements.size) {
      continue;
    }
    if (it == regions.end() || it->size > iter->size) {
      it = iter;
      if (it->size == memory_requirements.size) {
        break;
      }
    }
  }
  if (it == regions.end()) {
    return {};
  }

  PageRegion region = *it;

  if (region.size > memory_requirements.size) {
    /* If the region is larger than required, split it. */
    it->offset = region.offset + memory_requirements.size;
    it->size = region.size - memory_requirements.size;
    region.size = memory_requirements.size;
  }
  else {
    /* Otherwise, remove the region from the list. */
    regions.erase(it);
  }

  return region;
}

void VKTexturePool::PageHandle::release_region(PageRegion region)
{
  /* Find the first region after the released region. */
  auto it_next = regions.begin();
  while (it_next != regions.end() && it_next->offset < region.offset) {
    ++it_next;
  }
  /* Find the last region before the released region. */
  auto it_prev = it_next;
  if (it_prev != regions.begin()) {
    --it_prev;
  }

  /* Extend the previous region, if it connects to the released region. */
  bool extended_prev = false;
  if (it_prev != regions.end() && (it_prev->offset + it_prev->size) == region.offset) {
    extended_prev = true;
    it_prev->size += region.size;
  } 
  /* Extend the next region, if it connects to the released region. */
  bool extended_next = false;
  if (it_next != regions.end() && it_next->offset == (region.offset + region.size)) {
    extended_next = true;  
    it_next->offset = region.offset;
    it_next->size += region.size;
  } 

  if (extended_prev && extended_next) {
    /* If both previous/next regions were extended, we can merge them. */
    it_prev->size += it_next->size - region.size;
    regions.erase(it_next);
  }
  else if (!(extended_prev || extended_next)) {
    /* If neither regions were extended, they do not connect. Insert in the middle. */
    regions.insert(it_next, region);
  }
}

bool VKTexturePool::PageHandle::init(VkMemoryRequirements memory_requirements)
{
  VKDevice &device = VKBackend::get().device;
  VmaAllocationCreateInfo create_info = {};
  create_info.priority = 1.0f;
  create_info.memoryTypeBits = memory_requirements.memoryTypeBits;
  create_info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  VkResult result = vmaAllocateMemory(device.mem_allocator_get(),
                                      &memory_requirements,
                                      &create_info,
                                      &allocation,
                                      &allocation_info);

  regions.clear();
  regions.push_back(PageRegion{0ul, allocation_info.size});

  return result == VK_SUCCESS;
}

void VKTexturePool::PageHandle::free()
{
  VKDevice &device = VKBackend::get().device;
  /* TODO(not_mark): allocation needs to go to discard pool, but for that it needs to be tracked.
   * This is only OK right now because `max_unused_cycles_` is sufficiently large. */
  vmaFreeMemory(device.mem_allocator_get(), allocation);
  regions.clear();
}

bool VKTexturePool::TextureHandle::init(int2 extent,
                                        TextureFormat format,
                                        eGPUTextureUsage usage,
                                        const char *name)
{
  VKDevice &device = VKBackend::get().device;

  texture = new VKTexture(name);
  texture->w_ = extent.x;
  texture->h_ = extent.y;
  texture->d_ = 0;
  texture->format_ = format;
  texture->format_flag_ = to_format_flag(format);
  texture->type_ = GPU_TEXTURE_2D;
  texture->gpu_image_usage_flags_ = usage;

  /* R16G16F16 formats are typically not supported (<1%). */
  texture->device_format_ = format;
  if (texture->device_format_ == TextureFormat::SFLOAT_16_16_16) {
    texture->device_format_ = TextureFormat::SFLOAT_16_16_16_16;
  }
  if (texture->device_format_ == TextureFormat::SFLOAT_32_32_32) {
    texture->device_format_ = TextureFormat::SFLOAT_32_32_32_32;
  }

  /* Mirrors behavior in gpu::Texture::init_2d(...). */
  if ((texture->format_flag_ & (GPU_FORMAT_DEPTH_STENCIL | GPU_FORMAT_INTEGER)) == 0) {
    texture->sampler_state.filtering = GPU_SAMPLER_FILTERING_LINEAR;
  }

  /* Create a VkImage object. */
  VkImageCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  create_info.flags = to_vk_image_create(GPU_TEXTURE_2D, to_format_flag(format), usage);
  create_info.usage = to_vk_image_usage(usage, to_format_flag(format), false);
  create_info.format = to_vk_format(format);
  create_info.arrayLayers = 1;
  create_info.mipLevels = 1;
  create_info.imageType = VK_IMAGE_TYPE_2D;
  create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  create_info.extent.width = static_cast<uint32_t>(extent.x);
  create_info.extent.height = static_cast<uint32_t>(extent.y);
  create_info.extent.depth = 1u;
  VkResult result = vkCreateImage(
      device.vk_handle(), &create_info, nullptr, &(texture->vk_image_));

  return result == VK_SUCCESS;
}

void VKTexturePool::TextureHandle::free()
{
  /* The image is forwarded for discard, but the allocation is not. It is
   * safe to not unbind an image from an allocation in VMA when freeing it. */
  VKDiscardPool::discard_pool_get().discard_image(texture->vk_image_, VK_NULL_HANDLE);

  /* VKTexture destructor is skipped as `VKTexture::allocation_` is `VK_NULL_HANDLE`. */
  delete texture;
}

VKTexturePool::~VKTexturePool()
{
  for (const TextureHandle &handle : acquired_) {
    release_texture(wrap(handle.texture));
  }
  for (AllocationHandle &handle : pool_) {
    handle.free();
  }
}

Texture *VKTexturePool::acquire_texture(int2 extent,
                                        TextureFormat format,
                                        eGPUTextureUsage usage,
                                        const char *name)
{
  VKDevice &device = VKBackend::get().device;

  /* Generate debug label name, if one isn't passed in `name`. */
  std::string name_str;
  if (G.debug & G_DEBUG_GPU) {
    name_str = name ? name : fmt::format("TexFromPool_{}", acquired_.size());
  }

  /* Create texture object with no backing allocation, wrapped in `TextureHandle`. */
  TextureHandle texture_handle;
  texture_handle.alloc(extent, format, usage, name_str.c_str());

  /* Query the requirements for this specific image */
  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(
      device.vk_handle(), texture_handle.texture->vk_image_, &memory_requirements);

  /* Find a compatible region of allocated memory. */
  for (auto page : pages_) {
    auto region_opt = page.acquire_region(memory_requirements);
    if (region_opt) {
      texture_handle.page_handle = page;
      texture_handle.page_region = *region_opt;
      pages_.add_overwrite(page);
      break;
    }
  }

  /* If no compatible region was found, allocate new memory. */
  if (texture_handle.page_handle.allocation == VK_NULL_HANDLE) {
    /* TODO(not_mark): add some heuristic instead of just over-allocating by 4x. */
    VkMemoryRequirements allocation_requirements = memory_requirements;
    allocation_requirements.size *= 2u;

    PageHandle page_handle;
    page_handle.init(allocation_requirements);
    auto region_opt = page_handle.acquire_region(memory_requirements);

    pages_.add(page_handle);
    texture_handle.page_handle = page_handle;
    texture_handle.page_region = *region_opt;

    /* TODO(not_mark): remove */
    // std::printf("Allocated %zu\n", texture_handle.page_handle.allocation_info.size);
  }

  /* Compute the necessary offset into the allocation to satisfy alignment requirements. */
  VkDeviceSize aligned_offset = align_offset(
      0, allocation_handle.allocation_info.offset, memory_requirements.alignment);

  /* Bind VkImage to allocation. */
  VkResult bind_result = vmaBindImageMemory2(device.mem_allocator_get(),
                                             texture_handle.page_handle.allocation,
                                             texture_handle.page_region.offset,
                                             texture_handle.texture->vk_image_,
                                             nullptr);

  /* WATCH(not_mark): if the bind fails with e.g. VK_ERROR_UNKNOWN, VkMemoryRequirements are
   * likely not correctly satisfied. I'll keep the assert in for now, as the problem otherwise
   * incorrectly shows up in the render graph. */
  UNUSED_VARS(bind_result);
  BLI_assert_msg(bind_result == VK_SUCCESS,
                 "VKTexturePool::acquire failed on vmaBindImageMemory2.");

  debug::object_label(texture_handle.texture->vk_image_, texture_handle.texture->name_);
  device.resources.add_aliased_image(
      texture_handle.texture->vk_image_, false, texture_handle.texture->name_.c_str());

  /* TODO(not_mark): remove */
  // std::printf("Acquired (start=%zu, end=%zu)\n",
  //             texture_handle.page_region.offset,
  //             texture_handle.page_region.offset + texture_handle.page_region.size);

  acquired_.add(texture_handle);
  return wrap(texture_handle.texture);
}

void VKTexturePool::release_texture(Texture *tex)
{
  BLI_assert_msg(acquired_.contains({unwrap(tex)}),
                 "Unacquired texture passed to VKTexturePool::offset_users_count()");
  TextureHandle texture_handle = acquired_.lookup_key({unwrap(tex)});

  /* Move allocation back to `pool_`. */
  auto page_handle = pages_.lookup_key(texture_handle.page_handle);
  page_handle.release_region(texture_handle.page_region);
  page_handle.unused_cycles_count = 0;
  pages_.add_overwrite(page_handle);

  /* TODO(not_mark): remove */
  // std::printf("Released (start=%zu, end=%zu)\n",
  //             texture_handle.page_region.offset,
  //             texture_handle.page_region.offset + texture_handle.page_region.size);

  /* Clear out acquired texture object. */
  acquired_.remove(texture_handle);
  texture_handle.free();
}

void VKTexturePool::offset_users_count(Texture *tex, int offset)
{
  BLI_assert_msg(acquired_.contains({unwrap(tex)}),
                 "Unacquired texture passed to VKTexturePool::offset_users_count()");
  TextureHandle texture_handle = acquired_.lookup_key({unwrap(tex)});
  texture_handle.users_count += offset;
  acquired_.add_overwrite(texture_handle);
}

void VKTexturePool::reset(bool force_free)
{
#ifndef NDEBUG
  /* Iterate acquired textures, and ensure the internal counter equals 0; otherwise
   * this indicates a missing `::retain()` or `::release()`. */
  for (const TextureHandle &tex : acquired_) {
    BLI_assert_msg(tex.users_count == 0,
                   "Missing texture release/retain. Likely TextureFromPool::release(), "
                   "TextureFromPool::retain() or TexturePool::release_texture().");
  }
#endif

  VkDeviceSize total_pool_usage = 0;
  VkDeviceSize used_pool_usage = 0;

  uint texture_i = 0;
  for (TextureHandle texture : acquired_) {
    std::printf("Texture %d\n", texture_i);
    std::printf("\tRegion 0 (offset=%zu, size=%zu)\n",
                texture.page_region.offset,
                texture.page_region.size);
    used_pool_usage += texture.page_region.size;
    texture_i++;
  }

  uint page_i = 0;


  /* Reverse iterate unused allocations, to make sure we only reorder known good handles. */
  for (PageHandle handle : pages_) {
    if (handle.is_unused() && (handle.unused_cycles_count >= max_unused_cycles_) || force_free) {
      handle.free();
      pages_.remove(handle);
    }
    else {
      handle.unused_cycles_count++;
      pages_.add_overwrite(handle);
    }

    total_pool_usage += handle.allocation_info.size;

    uint list_i = 0;
    std::printf("Page %d (size=%zu)\n", page_i, handle.allocation_info.size);
    for (auto region : handle.regions) {
      std::printf("\tRegion %d (start=%zu, end=%zu)\n",
                  list_i,
                  region.offset,
                  region.offset + region.size);
      list_i++;
    }
    page_i++;
  }

  std::printf("Pool allocation: used=%zumb, total=%zumb\n", used_pool_usage / 1024 / 1024, total_pool_usage / 1024 / 1024);
}

}  // namespace blender::gpu
