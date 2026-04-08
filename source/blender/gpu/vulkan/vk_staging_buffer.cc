/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup gpu
 */
#include "vk_staging_buffer.hh"
#include "vk_backend.hh"
#include "vk_context.hh"

namespace blender::gpu {

VKStagingBuffer::VKStagingBuffer(const VKBuffer &device_buffer,
                                 VkDeviceSize device_buffer_offset,
                                 VkDeviceSize region_size)
    : device_buffer_(device_buffer),
      device_buffer_offset_(device_buffer_offset),
      region_size_(region_size == UINT64_MAX ? device_buffer.size_in_bytes() : region_size)
{
  VKDevice &device = VKBackend::get().device;

  if (!host_buffer_.is_allocated()) {
    host_buffer_.create(HOST_BUFFER_SIZE,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_AUTO,
                        VMA_ALLOCATION_CREATE_MAPPED_BIT |
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                        0.4f);
    debug::object_label(host_buffer_.vk_handle(), "StagingBuffer");
  }

  VkDeviceSize alignment =
      device.physical_device_properties_get().limits.optimalBufferCopyOffsetAlignment;

  local_offset_ = allocate_region(region_size_, alignment);
}

VkDeviceSize VKStagingBuffer::allocate_region(VkDeviceSize size, VkDeviceSize alignment)
{
  VKDevice &device = VKBackend::get().device;
  VKContext &context = *VKContext::get();
  std::scoped_lock lock(allocator_mutex_);

  write_offset_ = ceil_to_multiple_ul(write_offset_, alignment);

  if (write_offset_ + size > HOST_BUFFER_SIZE) {
    write_offset_ = 0;
  }

  TimelineValue current_timeline = device.submission_finished_timeline_get();

  if (prev_timeline_ < current_timeline) {
    oldest_actual_offset_ = write_offset_;
  }
  else if (write_offset_ + size > oldest_actual_offset_) {
    /* We have run out of the host buffer's memory, so we clear the host buffer, ensuring that
     * previous copies are done. */
    context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                               RenderGraphFlushFlags::RENEW_RENDER_GRAPH |
                               RenderGraphFlushFlags::WAIT_FOR_COMPLETION);
    oldest_actual_offset_ = 0;
    prev_timeline_ = current_timeline;
    write_offset_ = 0;
  }

  VkDeviceSize allocated_offset = write_offset_;
  write_offset_ += size;

  return allocated_offset;
}

void VKStagingBuffer::copy_to_device(VKContext &context, const void *data)
{
  BLI_assert(host_buffer_.is_allocated() && host_buffer_.is_mapped());

  host_buffer_.update_sub_immediately(local_offset_, region_size_, data);

  render_graph::VKCopyBufferNode::CreateInfo copy = {};
  copy.src_buffer = host_buffer_.vk_handle();
  copy.dst_buffer = device_buffer_.vk_handle();
  copy.region.srcOffset = local_offset_;
  copy.region.dstOffset = device_buffer_offset_;
  copy.region.size = region_size_;

  context.render_graph().add_node(copy);
}

void VKStagingBuffer::copy_from_device(VKContext &context)
{
  BLI_assert(host_buffer_.is_allocated() && host_buffer_.is_mapped());

  render_graph::VKCopyBufferNode::CreateInfo copy = {};
  copy.src_buffer = device_buffer_.vk_handle();
  copy.dst_buffer = host_buffer_.vk_handle();
  copy.region.srcOffset = device_buffer_offset_;
  copy.region.dstOffset = local_offset_;
  copy.region.size = region_size_;

  context.render_graph().add_node(copy);

  saved_timeline_value_ = context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                                                     RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
}

void VKStagingBuffer::read(void *data)
{
  BLI_assert(host_buffer_.is_allocated() && host_buffer_.is_mapped());

  VKDevice &device = VKBackend::get().device;

  device.wait_for_timeline(saved_timeline_value_);

  memcpy(data,
         static_cast<uint8_t *>(host_buffer_.mapped_memory_get()) + local_offset_,
         region_size_);

  saved_timeline_value_ = 0;
}

void VKStagingBuffer::free(VKDevice &device)
{
  if (host_buffer_.is_allocated()) {
    host_buffer_.free_immediately(device);
  }
  write_offset_ = 0;
}

}  // namespace blender::gpu
