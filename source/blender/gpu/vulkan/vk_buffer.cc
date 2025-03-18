/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_buffer.hh"
#include "vk_backend.hh"
#include "vk_context.hh"

namespace blender::gpu {

VKBuffer::~VKBuffer()
{
  if (is_allocated()) {
    free();
  }
}

bool VKBuffer::is_allocated() const
{
  return allocation_ != VK_NULL_HANDLE || vk_device_memory != VK_NULL_HANDLE;
}

void VKBuffer::import_host_pointer(size_t size,
                                   VkBufferUsageFlags buffer_usage,
                                   void *host_pointer)
{
  BLI_assert(!is_allocated());
  BLI_assert(vk_buffer_ == VK_NULL_HANDLE);
  BLI_assert(mapped_memory_ == nullptr);
  BLI_assert(vk_device_memory == VK_NULL_HANDLE);

  size_in_bytes_ = size;
  alloc_size_in_bytes_ = size;
  mapped_memory_ = host_pointer;

  VKDevice &device = VKBackend::get().device;

  /* Find the memory type that can be used to for the given host pointer. */
  VkMemoryHostPointerPropertiesEXT vk_memory_host_pointer_properties = {
      VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
  device.functions.vkGetMemoryHostPointerProperties(
      device.vk_handle(),
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
      host_pointer,
      &vk_memory_host_pointer_properties);

  const VkPhysicalDeviceMemoryProperties &external_memory_host_properties =
      device.physical_device_memory_properties_get();
  uint memory_type_index = 0;
  for (int index : IndexRange(external_memory_host_properties.memoryTypeCount)) {
    /* We need to check if the memory type is host visible.
     *
     * According to the Vulkan spec:
     *
     * `memoryTypeBits` should only include bits that identify memory types which are host visible.
     * Implementations may include bits that identify memory types which are not host visible.
     * Behavior for imported pointers of such types is defined by
     * VkImportMemoryHostPointerInfoEXT.
     */
    const VkMemoryType &vk_memory_type = external_memory_host_properties.memoryTypes[index];
    if (bool(vk_memory_host_pointer_properties.memoryTypeBits & (1 << index)) &&
        bool(vk_memory_type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    {
      memory_type_index = index;
      break;
    }
  }

  /* Import the memory */
  VkImportMemoryHostPointerInfoEXT vk_import_memory_host_pointer_info = {
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
      nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
      host_pointer};
  VkMemoryAllocateInfo vk_memory_allocate_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                  &vk_import_memory_host_pointer_info,
                                                  alloc_size_in_bytes_,
                                                  memory_type_index};
  vkAllocateMemory(device.vk_handle(), &vk_memory_allocate_info, nullptr, &vk_device_memory);

  /* Create the buffer and bind the imported memory to the buffer. */
  VkExternalMemoryBufferCreateInfo vk_external_memory_buffer_create_info = {
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
      nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
  VkBufferCreateInfo vk_buffer_create_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                              &vk_external_memory_buffer_create_info,
                                              0,
                                              alloc_size_in_bytes_,
                                              buffer_usage,
                                              VK_SHARING_MODE_EXCLUSIVE,
                                              0,
                                              nullptr};
  vkCreateBuffer(device.vk_handle(), &vk_buffer_create_info, nullptr, &vk_buffer_);
  vkBindBufferMemory(device.vk_handle(), vk_buffer_, vk_device_memory, 0);

  device.resources.add_buffer(vk_buffer_);
}

bool VKBuffer::create(size_t size_in_bytes,
                      VkBufferUsageFlags buffer_usage,
                      VkMemoryPropertyFlags required_flags,
                      VkMemoryPropertyFlags preferred_flags,
                      VmaAllocationCreateFlags allocation_flags)
{
  BLI_assert(!is_allocated());
  BLI_assert(vk_buffer_ == VK_NULL_HANDLE);
  BLI_assert(mapped_memory_ == nullptr);
  BLI_assert(vk_device_memory == VK_NULL_HANDLE);

  size_in_bytes_ = size_in_bytes;
  alloc_size_in_bytes_ = ceil_to_multiple_ul(max_ulul(size_in_bytes_, 16), 16);
  VKDevice &device = VKBackend::get().device;

  VmaAllocator allocator = device.mem_allocator_get();
  VkBufferCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  create_info.flags = 0;
  /*
   * Vulkan doesn't allow empty buffers but some areas (DrawManager Instance data, PyGPU) create
   * them.
   */
  create_info.size = alloc_size_in_bytes_;
  create_info.usage = buffer_usage;
  /* We use the same command queue for the compute and graphics pipeline, so it is safe to use
   * exclusive resource handling. */
  create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create_info.queueFamilyIndexCount = 1;
  const uint32_t queue_family_indices[1] = {device.queue_family_get()};
  create_info.pQueueFamilyIndices = queue_family_indices;

  VmaAllocationCreateInfo vma_create_info = {};
  vma_create_info.flags = allocation_flags;
  vma_create_info.priority = 1.0f;
  vma_create_info.requiredFlags = required_flags;
  vma_create_info.preferredFlags = preferred_flags;
  vma_create_info.usage = VMA_MEMORY_USAGE_AUTO;

  VkResult result = vmaCreateBuffer(
      allocator, &create_info, &vma_create_info, &vk_buffer_, &allocation_, nullptr);
  if (result != VK_SUCCESS) {
    return false;
  }

  device.resources.add_buffer(vk_buffer_);

  vmaGetAllocationMemoryProperties(allocator, allocation_, &vk_memory_property_flags_);

  if (vk_memory_property_flags_ & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    return map();
  }
  return true;
}

void VKBuffer::update_immediately(const void *data) const
{
  update_sub_immediately(0, size_in_bytes_, data);
}

void VKBuffer::update_sub_immediately(size_t start_offset,
                                      size_t data_size,
                                      const void *data) const
{
  BLI_assert_msg(is_mapped(), "Cannot update a non-mapped buffer.");
  memcpy(static_cast<uint8_t *>(mapped_memory_) + start_offset, data, data_size);
}

void VKBuffer::update_render_graph(VKContext &context, void *data) const
{
  BLI_assert(size_in_bytes_ <= 65536 && size_in_bytes_ % 4 == 0);
  render_graph::VKUpdateBufferNode::CreateInfo update_buffer = {};
  update_buffer.dst_buffer = vk_buffer_;
  update_buffer.data_size = size_in_bytes_;
  update_buffer.data = data;
  context.render_graph().add_node(update_buffer);
}

void VKBuffer::flush() const
{
  const VKDevice &device = VKBackend::get().device;
  VmaAllocator allocator = device.mem_allocator_get();
  vmaFlushAllocation(allocator, allocation_, 0, max_ulul(size_in_bytes(), 1));
}

void VKBuffer::clear(VKContext &context, uint32_t clear_value)
{
  render_graph::VKFillBufferNode::CreateInfo fill_buffer = {};
  fill_buffer.vk_buffer = vk_buffer_;
  fill_buffer.data = clear_value;
  fill_buffer.size = alloc_size_in_bytes_;
  context.render_graph().add_node(fill_buffer);
}

void VKBuffer::async_flush_to_host(VKContext &context)
{
  BLI_assert(async_timeline_ == 0);
  context.rendering_end();
  context.descriptor_set_get().upload_descriptor_sets();
  async_timeline_ = context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                                               RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
}

void VKBuffer::read_async(VKContext &context, void *data)
{
  BLI_assert_msg(is_mapped(), "Cannot read a non-mapped buffer.");
  if (async_timeline_ == 0) {
    async_flush_to_host(context);
  }
  VKDevice &device = VKBackend::get().device;
  device.wait_for_timeline(async_timeline_);
  async_timeline_ = 0;
  memcpy(data, mapped_memory_, size_in_bytes_);
}

void VKBuffer::read(VKContext &context, void *data) const
{
  BLI_assert_msg(is_mapped(), "Cannot read a non-mapped buffer.");
  BLI_assert(async_timeline_ == 0);
  context.rendering_end();
  context.descriptor_set_get().upload_descriptor_sets();
  context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                             RenderGraphFlushFlags::WAIT_FOR_COMPLETION |
                             RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
  memcpy(data, mapped_memory_, size_in_bytes_);
}

void *VKBuffer::mapped_memory_get() const
{
  BLI_assert_msg(is_mapped(), "Cannot access a non-mapped buffer.");
  return mapped_memory_;
}

bool VKBuffer::is_mapped() const
{
  return mapped_memory_ != nullptr;
}

bool VKBuffer::map()
{
  if (mapped_memory_ == VK_NULL_HANDLE && allocation_ != VK_NULL_HANDLE) {
    const VKDevice &device = VKBackend::get().device;
    VmaAllocator allocator = device.mem_allocator_get();
    vmaMapMemory(allocator, allocation_, &mapped_memory_);
  }
  return mapped_memory_ != nullptr;
}

void VKBuffer::unmap()
{
  BLI_assert(is_mapped());
  if (allocation_ != VK_NULL_HANDLE) {
    const VKDevice &device = VKBackend::get().device;
    VmaAllocator allocator = device.mem_allocator_get();
    vmaUnmapMemory(allocator, allocation_);
    mapped_memory_ = nullptr;
  }
}

bool VKBuffer::free()
{
  if (is_mapped()) {
    unmap();
  }

  VKDiscardPool::discard_pool_get().discard_buffer(vk_buffer_, allocation_);

  allocation_ = VK_NULL_HANDLE;
  vk_buffer_ = VK_NULL_HANDLE;

  return true;
}

void VKBuffer::free_immediately(VKDevice &device)
{
  BLI_assert(vk_buffer_ != VK_NULL_HANDLE);
  BLI_assert(allocation_ != VK_NULL_HANDLE);
  if (is_mapped()) {
    unmap();
  }
  device.resources.remove_buffer(vk_buffer_);
  vmaDestroyBuffer(device.mem_allocator_get(), vk_buffer_, allocation_);
  allocation_ = VK_NULL_HANDLE;
  vk_buffer_ = VK_NULL_HANDLE;
}

}  // namespace blender::gpu
