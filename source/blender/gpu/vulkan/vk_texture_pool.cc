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

#include "BKE_global.hh"

#include "CLG_log.h"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.vulkan"};

static VkDeviceSize align_size(VkDeviceSize size, VkDeviceSize alignment)
{
  return (size - 1u + alignment) & -alignment;
}

std::optional<VKTexturePool::Segment> VKTexturePool::AllocationHandle::acquire(
    VkMemoryRequirements requirements)
{
  /* `memoryType` uses 0 as special value to indicate no restrictions.
   * If there are restrictions, we check against `memoryTypeBits`.  */
  if (allocation_info.memoryType != 0 &&
      !bool(requirements.memoryTypeBits & allocation_info.memoryType))
  {
    return {};
  }

  /* Memory alignment can vary between images, and segments offset into the
   * allocation likewise influence this. */

  /* Find the smallest segment of compatible size. */
  auto it = segments.end();
  for (auto iter = segments.begin(); iter != segments.end(); ++iter) {
    /* Align to segment at start. */
    VkDeviceSize aligned_offset = align_size(iter->offset, requirements.alignment);
    VkDeviceSize remaining_size = iter->size - (aligned_offset - iter->offset);
    if (remaining_size < requirements.size) {
      continue;
    }
    if (it == segments.end() || it->size > iter->size) {
      it = iter;
    }
  }
  if (it == segments.end()) {
    return {};
  }
  
  // if (segment.size > requirements.size) {
  /* Alignment can lead to an offset to the segment interior. */
  VkDeviceSize aligned_offset = align_size(it->offset, requirements.alignment);
  VkDeviceSize remaining_size = it->size - (aligned_offset - it->offset);

  /* Identify segments before/at/after the acquired segment. */
  Segment segment_prev = {it->offset, aligned_offset - it->offset};
  Segment segment = { aligned_offset, requirements.size };
  Segment segment_next = {aligned_offset + requirements.size,
                          remaining_size - requirements.size};

  if (segment_prev.size > 0 && segment_next.size > 0) {
    *it = segment_next;
    segments.insert(it, segment_prev);
  } else if (segment_prev.size > 0) {
    *it = segment_prev;
  } else if (segment_next.size > 0) {
    *it = segment_next;
  } else {
    segments.erase(it);
  }

  return segment;
}

void VKTexturePool::AllocationHandle::release(Segment segment)
{
  /* Find the segment after the released segment. */
  auto it_next = segments.begin();
  while (it_next != segments.end() && it_next->offset < segment.offset) {
    ++it_next;
  }
  /* Find the segment before the released segment. */
  auto it_prev = it_next;
  if (it_prev != segments.begin()) {
    --it_prev;
  }

  /* Extend the previous segment, if it connects to the released segment. */
  bool extended_prev = false;
  if (it_prev != segments.end() && (it_prev->offset + it_prev->size) == segment.offset) {
    extended_prev = true;
    it_prev->size += segment.size;
  }
  /* Extend the next segment, if it connects to the released segment. */
  bool extended_next = false;
  if (it_next != segments.end() && it_next->offset == (segment.offset + segment.size)) {
    extended_next = true;
    it_next->offset = segment.offset;
    it_next->size += segment.size;
  }

  if (extended_prev && extended_next) {
    /* If both previous/next segment were extended, we can merge them. */
    it_prev->size += it_next->size - segment.size;
    segments.erase(it_next);
  }
  else if (!(extended_prev || extended_next)) {
    /* If neither segment were extended, they do not connect. Insert in the middle. */
    segments.insert(it_next, segment);
  }
}

bool VKTexturePool::AllocationHandle::init(VkMemoryRequirements memory_requirements)
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

  /* Start with a single, fully sized segment. */
  segments = {{0ul, allocation_info.size}};

  return result == VK_SUCCESS;
}

void VKTexturePool::AllocationHandle::free()
{
  VKDevice &device = VKBackend::get().device;
  /* TODO(not_mark): allocation needs to go to discard pool, but for that it needs to be tracked.
   * This is only OK right now because `max_unused_cycles_` is sufficiently large. */
  vmaFreeMemory(device.mem_allocator_get(), allocation);
  segments = {};
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
  for (AllocationHandle &handle : allocations_) {
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

  /* Query the requirements for this specific image. We expand size up to alignment,
   * so the segment after our acquired segment is also aligned at the start. */
  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(
      device.vk_handle(), texture_handle.texture->vk_image_, &memory_requirements);

  /* Find a compatible segment of allocated memory. */
  for (auto handle : allocations_) {
    auto segment_opt = handle.acquire(memory_requirements);
    if (segment_opt) {
      texture_handle.allocation_handle = handle;
      texture_handle.segment = *segment_opt;
      allocations_.add_overwrite(handle);
      break;
    }
  }

  /* If no compatible region was found, allocate new memory. */
  if (texture_handle.allocation_handle.allocation == VK_NULL_HANDLE) {
    VkMemoryRequirements allocation_requirements = memory_requirements;
    allocation_requirements.size = std::max(allocation_size, allocation_requirements.size);

    AllocationHandle handle;
    handle.init(allocation_requirements);
    auto region_opt = handle.acquire(memory_requirements);

    allocations_.add(handle);
    texture_handle.allocation_handle = handle;
    texture_handle.segment = *region_opt;
  }

  /* Compute the necessary offset into the allocation to satisfy alignment requirements. */
  VkDeviceSize aligned_offset = align_offset(
      0, allocation_handle.allocation_info.offset, memory_requirements.alignment);

  /* Bind VkImage to allocation. */
  VkResult bind_result = vmaBindImageMemory2(device.mem_allocator_get(),
                                             texture_handle.allocation_handle.allocation,
                                             texture_handle.segment.offset,
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

  if (G.debug & G_DEBUG_GPU) {
    /* Accumulate usage data for debug log. Maximum is stored. */
    current_usage_data_.acquired_segment_size += texture_handle.segment.size;
    current_usage_data_.acquired_segment_size_max = std::max(
        current_usage_data_.acquired_segment_size_max, current_usage_data_.acquired_segment_size);
  }

  acquired_.add(texture_handle);
  return wrap(texture_handle.texture);
}

void VKTexturePool::release_texture(Texture *tex)
{
  BLI_assert_msg(acquired_.contains({unwrap(tex)}),
                 "Unacquired texture passed to VKTexturePool::offset_users_count()");
  TextureHandle texture_handle = acquired_.lookup_key({unwrap(tex)});

  if (G.debug & G_DEBUG_GPU) {
    current_usage_data_.acquired_segment_size -= texture_handle.segment.size;
  }

  /* Move allocation back to `pool_`. */
  auto page_handle = allocations_.lookup_key(texture_handle.allocation_handle);
  page_handle.release(texture_handle.segment);
  page_handle.unused_cycles_count = 0;
  allocations_.add_overwrite(page_handle);

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

  /* Reverse iterate unused allocations, to make sure we only reorder known good handles. */
  for (AllocationHandle handle : allocations_) {
    if (handle.is_unused() && (handle.unused_cycles_count >= max_unused_cycles_ || force_free)) {
      handle.free();
      allocations_.remove(handle);
    }
    else {
      handle.unused_cycles_count++;
      allocations_.add_overwrite(handle);
    }
  }

  if (G.debug & G_DEBUG_GPU) {
    /* Log debug usage data if it differs from the last `::reset()`. */
    current_usage_data_.allocation_count = allocations_.size();
    if (!(previous_usage_data_ == current_usage_data_)) {
      log_usage_data();
    }

    /* Reset usage data; don't forget to add up persistent textures to current usage. */
    previous_usage_data_ = current_usage_data_;
    current_usage_data_ = {};
    for (const TextureHandle &tex : acquired_) {
      current_usage_data_.acquired_segment_size += tex.segment.size;
    }
  }
}

void VKTexturePool::log_usage_data()
{
  VkDeviceSize total_allocation_size = 0;
  for (const auto &handle : allocations_) {
    total_allocation_size += handle.allocation_info.size;
  }
  float ratio = static_cast<float>(current_usage_data_.acquired_segment_size_max) /
                static_cast<float>(total_allocation_size);

  CLOG_TRACE(&LOG,
             "VKTexturePool uses %zu/%zu mb (%.1f%% of %li allocations)",
             current_usage_data_.acquired_segment_size_max >> 20,
             total_allocation_size >> 20,
             ratio * 100.0f,
             current_usage_data_.allocation_count);
}

}  // namespace blender::gpu
