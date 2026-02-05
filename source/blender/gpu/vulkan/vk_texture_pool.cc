/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_capabilities.hh"
// #include "GPU_debug.hh"

#include <tuple>

#include "vk_backend.hh"
#include "vk_texture.hh"
#include "vk_texture_pool.hh"

#include "fmt/format.h"

#include "BKE_global.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"gpu.vulkan"};

namespace detail {
/* Pass non-hardcoded arguments of VkImageCreateInfo into tuple of lvalues. */
/* TODO(not_mark): add keep in sync warning. */
constexpr auto tie(const VkImageCreateInfo &info)
{
  return std::tie(info.format, info.flags, info.usage, info.extent.width, info.extent.height);
}
}  // namespace detail

/* Implements DefaultHash over non-hardcoded arguments of VkImageCreateInfo. */
/* TODO(not_mark): add keep in sync warning. */
template<> struct DefaultHash<VkImageCreateInfo> {
  constexpr uint64_t operator()(const VkImageCreateInfo &value) const
  {
    const auto &[_1, _2, _3, _4, _5] = detail::tie(value);
    return get_default_hash(_1, _2, _3, _4, _5);
  }
};

namespace gpu {

uint64_t VKImageInfo::hash() const
{
  return get_default_hash(create_info, allocation, allocation_local_segment);
}

bool VKImageInfo::operator==(const VKImageInfo &o) const
{
  return detail::tie(create_info) == detail::tie(o.create_info) && allocation == o.allocation &&
         allocation_local_segment == o.allocation_local_segment;
}

VkImage VKImageCache::get_or_create(const VKImageInfo &info)
{
  VKDevice &device = VKBackend::get().device;

  /* If a bound VkImage handle exists in the map, reset its counter
   * and return it. */
  VKImageHandle *ptr = cache_.lookup_ptr(info);
  if (ptr) {
    ptr->unused_cycles_count = 0;
    return ptr->image;
  }

  /* Otherwise, assemble VkImageCreateInfo and create a new image. */
  VkImage image;
  VkResult create_result = vkCreateImage(device.vk_handle(), &info.create_info, nullptr, &image);
  UNUSED_VARS(create_result);
  BLI_assert(create_result == VK_SUCCESS);

  /* Then, bindd to the provided allocation */
  VmaAllocator allocator = device.mem_allocator_get();
  VkResult bind_result = vmaBindImageMemory2(
      allocator, info.allocation, info.allocation_local_segment.offset, image, nullptr);
  UNUSED_VARS(bind_result);
  BLI_assert(bind_result == VK_SUCCESS);

  /* Insert handle into cache. */
  cache_.add_new(info, {.image = image});

  /* TODO(not_mark): pass in name */
  device.resources.add_aliased_image(image, false, "uhh");

  return image;
}

void VKImageCache::reset(bool force_reset)
{
  /* Track VKImageInfo keys corresponding to VkImages that need to be deleted. */
  Vector<VKImageInfo> unused_images;

  /* Iterate cache; add keys of images hitting `unused_cycles_count` to `unused_images`.  */
  for (decltype(cache_)::MutableItem item : cache_.items()) {
    item.value.unused_cycles_count++;
    if (force_reset == true || item.value.unused_cycles_count >= max_unused_cycles_) {
      unused_images.append(item.key);
    }
  }

  /* Remove unused images from cache, and forward VkImage handles to discard pool.  */
  for (const VKImageInfo &key : unused_images) {
    VKImageHandle handle = cache_.pop(key);
    VKDiscardPool::discard_pool_get().discard_image(handle.image, VK_NULL_HANDLE);
  }
}

std::optional<VKDeviceSegment> VKTexturePool::AllocationHandle::acquire(
    VkMemoryRequirements requirements)
{
  /* `memoryType` uses 0 as special value to indicate no memory type restrictions.n
   * If there are restrictions, we check as a mask against `memoryTypeBits`. */
  uint32_t memory_type_bit = 1u << allocation_info.memoryType;
  if (allocation_info.memoryType != 0 && !bool(requirements.memoryTypeBits & memory_type_bit)) {
    return {};
  }

  /* Find the first compatible segment. If a segment is found, we keep the iterator
   * to modify the existing segment in the list, as it may be shrunk or split. */
  auto found_segment = std::find_if(
      segments.begin(), segments.end(), [&](const VKDeviceSegment &segment) {
        VkDeviceSize aligned_offset = ceil_to_multiple_ul(segment.offset, requirements.alignment);
        VkDeviceSize remaining_size = segment.size - (aligned_offset - segment.offset);
        return
            /* Check: the aligned offset does not lie past the segment's end. */
            aligned_offset < segment.offset + segment.size &&
            /* Check: the segment's remaining size is large enough. */
            remaining_size >= requirements.size;
      });
  if (found_segment == segments.end()) {
    return {};
  }

  /* The return segment is split from the found segment, starting at the aligned offset. This
   * implies there are now segments before/after it. */
  VKDeviceSegment segment = {ceil_to_multiple_ul(found_segment->offset, requirements.alignment),
                             requirements.size};
  VKDeviceSegment segment_prev = {found_segment->offset, segment.offset - found_segment->offset};
  VKDeviceSegment segment_next = {segment.offset + segment.size,
                                  found_segment->size - segment.size - segment_prev.size};

  /* Depending on the segments before/after, we shrink/split/remove the stored segment. */
  if (segment_prev.size > 0 && segment_next.size > 0) {
    *found_segment = segment_next;
    segments.insert(found_segment, segment_prev);
  }
  else if (segment_prev.size > 0) {
    *found_segment = segment_prev;
  }
  else if (segment_next.size > 0) {
    *found_segment = segment_next;
  }
  else {
    segments.erase(found_segment);
  }

  return segment;
}

void VKTexturePool::AllocationHandle::release(VKDeviceSegment segment)
{
  /* Find the segments directly before/after the released segment, if they exist. */
  auto segment_next = std::find_if(
      segments.begin(), segments.end(), [segment](VKDeviceSegment next) {
        return segment.offset < next.offset;
      });
  auto segment_prev = segment_next;
  if (segment_prev != segments.begin()) {
    --segment_prev;
  }

  /* Extend the previous/next segment, if they connect to the released segment. */
  bool extend_prev = segment_prev != segments.end() &&
                     segment.offset == (segment_prev->offset + segment_prev->size);
  bool extend_next = segment_next != segments.end() &&
                     segment_next->offset == (segment.offset + segment.size);
  if (extend_prev) {
    segment_prev->size += segment.size;
  }
  if (extend_next) {
    segment_next->offset = segment.offset;
    segment_next->size += segment.size;
  }

  /* If both segments are extended, we join them. If neither was extended, we
   * insert the released segment in between, as it doesn't connect to either. */
  if (extend_prev && extend_next) {
    segment_prev->size += segment_next->size - segment.size;
    segments.erase(segment_next);
  }
  else if (!(extend_prev || extend_next)) {
    segments.insert(segment_next, segment);
  }
}

void VKTexturePool::AllocationHandle::alloc(VkMemoryRequirements memory_requirements)
{
  VKDevice &device = VKBackend::get().device;

  VmaAllocationCreateInfo create_info = {};
  create_info.priority = 1.0f;
  create_info.memoryTypeBits = memory_requirements.memoryTypeBits;
  create_info.flags = VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT;
  create_info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  VkResult result = vmaAllocateMemory(device.mem_allocator_get(),
                                      &memory_requirements,
                                      &create_info,
                                      &allocation,
                                      &allocation_info);

  /* WATCH(not_mark): will remove asserts when pool is a bit more mature. */
  UNUSED_VARS(result);
  BLI_assert(result == VK_SUCCESS);

  /* Start with a single segment, sized to the full range of the allocation. */
  segments = {{allocation_info.offset, allocation_info.size}};
}

void VKTexturePool::AllocationHandle::free()
{
  VKDevice &device = VKBackend::get().device;
  /* TODO(not_mark): allocation needs to go to discard pool, but for that it needs to be tracked.
   * This is only OK right now because `max_unused_cycles_` is sufficiently large. */
  vmaFreeMemory(device.mem_allocator_get(), allocation);
  segments = {};
}

void VKTexturePool::TextureHandle::alloc(int2 extent,
                                         TextureFormat format,
                                         eGPUTextureUsage usage,
                                         const char *name)
{
  // VKDevice &device = VKBackend::get().device;

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
}

void VKTexturePool::TextureHandle::free()
{
  /* The image is forwarded for discard, but the allocation is not. It is
   * safe to not unbind an image from an allocation in VMA when freeing it. */
  // VKDiscardPool::discard_pool_get().discard_image(texture->vk_image_, VK_NULL_HANDLE);

  /* VKTexture destructor is skipped as `VKTexture::allocation_` is `VK_NULL_HANDLE`. */
  delete texture;
}

VKTexturePool::~VKTexturePool()
{
  for (const TextureHandle &handle : acquired_) {
    release_texture(wrap(handle.texture));
  }
  for (const ImageHandle &handle : free_) {
    VKDiscardPool::discard_pool_get().discard_image(handle.image, VK_NULL_HANDLE);
  }
  for (AllocationHandle handle : allocations_) {
    handle.free();
  }
}

Texture *VKTexturePool::acquire_texture(int2 extent,
                                        TextureFormat format,
                                        eGPUTextureUsage usage,
                                        const char *name)
{
  // GPU_debug_group_begin("VKTexturePool::acquire_texture");

  VKDevice &device = VKBackend::get().device;

  /* Generate debug label name, if one isn't passed in `name`. */
  std::string name_str;
  if (G.debug & G_DEBUG_GPU) {
    name_str = name ? name : fmt::format("TexFromPool_{}", acquired_.size());
  }

  /* Info object; either we create an image, or simply query memory requirements with this. */
  VkImageCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  create_info.pNext = nullptr;
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
  const uint32_t queue_family_indices[1] = {device.queue_family_get()};
  create_info.queueFamilyIndexCount = 1;
  create_info.pQueueFamilyIndices = queue_family_indices;

  /* Query memory requirements. */
  VkImage image = VK_NULL_HANDLE;
  VkMemoryRequirements2 memory_requirements = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
  if (device.extensions_get().maintenance4) {
    /* If `VK_KHR_maintenance4` is available, we create requirements from
     * VkImageCreateInfo, and delay creating a VkImage handle. */
    VkDeviceImageMemoryRequirements requirements_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
        .pNext = nullptr,
        .pCreateInfo = &create_info,
        .planeAspect = VK_IMAGE_ASPECT_NONE,
    };
    device.functions.vkGetDeviceImageMemoryRequirements(
        device.vk_handle(), &requirements_info, &memory_requirements);
  }
  else {
    /* If `VK_KHR_maintenance4` is not available, we'll have to create an image handle
     * either way. If a matching handle is already cached, we'll discard it. */
    VkResult result = vkCreateImage(device.vk_handle(), &create_info, nullptr, &image);
    UNUSED_VARS(result);
    BLI_assert(result == VK_SUCCESS);

    VkImageMemoryRequirementsInfo2 requirements_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = nullptr,
        .image = image,
    };
    vkGetImageMemoryRequirements2(device.vk_handle(), &requirements_info, &memory_requirements);
  }

  /* Create texture object with no backing allocation, wrapped in `TextureHandle`. */
  TextureHandle texture_handle;
  texture_handle.alloc(extent, format, usage, name_str.c_str());

  /* Find a compatible segment of allocated memory. */
  for (AllocationHandle handle : allocations_) {
    std::optional<VKDeviceSegment> segment_opt = handle.acquire(
        memory_requirements.memoryRequirements);
    if (segment_opt) {
      texture_handle.allocation = handle.allocation;
      texture_handle.local_segment = segment_opt.value();
      texture_handle.local_segment.offset -= handle.allocation_info.offset;
      allocations_.add_overwrite(handle);
      break;
    }
  }

  /* If no compatible region was found, allocate new memory. */
  if (texture_handle.allocation == VK_NULL_HANDLE) {
    VkMemoryRequirements allocation_requirements = memory_requirements.memoryRequirements;
    allocation_requirements.size = std::max(allocation_size, allocation_requirements.size);

    AllocationHandle handle;
    handle.alloc(allocation_requirements);

    std::optional<VKDeviceSegment> segment_opt = handle.acquire(
        memory_requirements.memoryRequirements);
    if (segment_opt) {
      allocations_.add(handle);
      texture_handle.allocation = handle.allocation;
      texture_handle.local_segment = segment_opt.value();
      texture_handle.local_segment.offset -= handle.allocation_info.offset;
    }
    else {
      BLI_assert_unreachable();
    }
  }

  /* Next, find an existing VkImage handle, bound to this segment of the allocation. */
  ImageHandle image_handle = {.allocation = texture_handle.allocation,
                              .local_segment = texture_handle.local_segment,
                              .format = create_info.format,
                              .flags = create_info.flags,
                              .usage = create_info.usage,
                              .extent = create_info.extent};
  if (free_.contains(image_handle)) {
    /* If `VK_KHR_maintenance4` is not available, we'll have to destroy this handle now. */
    if (image != VK_NULL_HANDLE) {
      vkDestroyImage(device.vk_handle(), image, nullptr);
    }

    /* Copy image handle from cache. */
    image_handle.image = free_.lookup_key(image_handle).image;
    texture_handle.texture->vk_image_ = image_handle.image;
  }
  else {
    /* We either just created - or now create - a new image handle, and then bind it. */
    if (image == VK_NULL_HANDLE) {
      VkResult result = vkCreateImage(device.vk_handle(), &create_info, nullptr, &image);
      UNUSED_VARS(result);
      BLI_assert(result == VK_SUCCESS);
    }
    image_handle.image = image;
    texture_handle.texture->vk_image_ = image_handle.image;

    /* Bind VkImage to allocation and set as backing image for texture. */
    VkResult result = vmaBindImageMemory2(device.mem_allocator_get(),
                                          texture_handle.allocation,
                                          texture_handle.local_segment.offset,
                                          image_handle.image,
                                          nullptr);

    device.resources.add_aliased_image(
        image_handle.image, false, texture_handle.texture->name_.c_str());

    /* WATCH(not_mark): if the bind fails with e.g. VK_ERROR_UNKNOWN, VkMemoryRequirements are
     * likely not correctly satisfied. I'll keep the assert in for now, as the problem otherwise
     * incorrectly shows up in the render graph. */
    UNUSED_VARS(result);
    BLI_assert_msg(result == VK_SUCCESS, "VKTexturePool::acquire failed on vmaBindImageMemory2.");
  }

  /* Reset cached unused_cycles_count to 0. */
  free_.add_overwrite(image_handle);

  debug::object_label(texture_handle.texture->vk_image_, texture_handle.texture->name_);

  if (G.debug & G_DEBUG_GPU) {
    /* Accumulate usage data for debug log. */
    current_usage_data_.acquired_segment_size += texture_handle.local_segment.size;
    current_usage_data_.acquired_segment_size_max = std::max(
        current_usage_data_.acquired_segment_size_max, current_usage_data_.acquired_segment_size);
  }

  acquired_.add(texture_handle);

  // GPU_debug_group_end();

  return wrap(texture_handle.texture);
}

void VKTexturePool::release_texture(Texture *tex)
{
  BLI_assert_msg(acquired_.contains({unwrap(tex)}),
                 "Unacquired texture passed to VKTexturePool::offset_users_count()");
  TextureHandle texture_handle = acquired_.lookup_key({unwrap(tex)});

  if (G.debug & G_DEBUG_GPU) {
    current_usage_data_.acquired_segment_size -= texture_handle.local_segment.size;
  }

  /* Move allocation back to `pool_`. */
  AllocationHandle page_handle = allocations_.lookup_key(
      AllocationHandle{.allocation = texture_handle.allocation});
  texture_handle.local_segment.offset += page_handle.allocation_info.offset;
  page_handle.release(texture_handle.local_segment);
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
      current_usage_data_.acquired_segment_size += tex.local_segment.size;
    }
  }
}

void VKTexturePool::log_usage_data()
{
  VkDeviceSize total_allocation_size = 0;
  for (const AllocationHandle &handle : allocations_) {
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

}  // namespace gpu
}  // namespace blender
