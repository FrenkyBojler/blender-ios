/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_staging_buffer.hh"
#include "render_graph/vk_resource_state_tracker.hh"
#include "vk_context.hh"
#include "BLI_binary_search.hh"
#include "CLG_log.h"

static CLG_LogRef LOG = {"gpu.vulkan"};

namespace blender::gpu {

VKStagingBuffer::VKStagingBuffer(const VKBuffer &device_buffer,
                                 Direction direction,
                                 VkDeviceSize device_buffer_offset,
                                 VkDeviceSize region_size)
    : device_buffer_(device_buffer),
      direction_(direction),
      device_buffer_offset_(device_buffer_offset),
      region_size_(region_size == UINT64_MAX ? device_buffer.size_in_bytes() : region_size)
{
}

void VKStagingBuffer::init(VKDevice &device)
{
  if (host_buffer_.is_allocated()) {
    return;
  }

  /* Assume that the generic queue owns the device buffer */
  is_generic_queue_needed_ = device.transfer_queue_family_get() !=
                             device.generic_queue_family_get();

  vkGetDeviceQueue(device.vk_handle(),
                   device.transfer_queue_family_get(),
                   device.transfer_queue_index_get(),
                   &transfer_queue_);

  VkCommandPoolCreateInfo command_pool_create_info{};
  command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  command_pool_create_info.queueFamilyIndex = VKBackend::get().device.transfer_queue_family_get();

  if (vkCreateCommandPool(
          device.vk_handle(), &command_pool_create_info, nullptr, &command_pool_) != VK_SUCCESS)
  {
    CLOG_ERROR(
        &LOG,
        "Unable to create command pool for the staging buffer. The staging buffer is invalid.");
    return;
  }

  VkSemaphoreCreateInfo semaphore_create_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

  if (vkCreateSemaphore(device.vk_handle(),
                        &semaphore_create_info,
                        nullptr,
                        &render_graph_finished_semaphore_) != VK_SUCCESS)
  {
    cleanup(device.vk_handle());
    CLOG_ERROR(&LOG, "Unable to create semaphore for the staging buffer. The staging buffer is invalid.");
    return;
  }
  VkSemaphoreTypeCreateInfo semaphore_type_create_info = {
      VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
  semaphore_create_info.pNext = &semaphore_type_create_info;

  if (vkCreateSemaphore(
          device.vk_handle(), &semaphore_create_info, nullptr, &timeline_semaphore_) != VK_SUCCESS)
  {
    cleanup(device.vk_handle());
    CLOG_ERROR(&LOG, "Unable to create semaphore for the staging buffer. The staging buffer is invalid.");
    return;
  }


  host_buffer_.create(HOST_BUFFER_SIZE,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_MAPPED_BIT |
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                      0.4f);

  if (!host_buffer_.is_allocated()) {
    cleanup(device.vk_handle());
    CLOG_ERROR(&LOG, "Unable to allocate the staging buffer. The staging buffer is invalid.");
    return;
  }

  debug::object_label(host_buffer_.vk_handle(), "StagingBuffer");

  /** We don't need to track the host buffer */
  device.resources.remove_buffer(host_buffer_.vk_handle());
}

void VKStagingBuffer::deinit(VKDevice &device)
{
  if (!host_buffer_.is_allocated()) {
    return;
  }

  vkQueueWaitIdle(transfer_queue_);

  cleanup(device.vk_handle());

  device.resources.add_buffer(host_buffer_.vk_handle());
  host_buffer_.free_immediately(device);

  timeline_value_ = 0;
}

void VKStagingBuffer::cleanup(VkDevice device)
{
  for (CommandBufferDescriptor &desc : staging_command_buffers_unused()) {
    vkDestroySemaphore(device, desc.binary_semaphore, nullptr);
  }
  for (CommandBufferDescriptor &desc : staging_command_buffers_in_use()) {
    vkDestroySemaphore(device, desc.binary_semaphore, nullptr);
  }
  if (command_pool_ != VK_NULL_HANDLE) {
    vkDestroyCommandPool(device, command_pool_, nullptr);
  }
  if (render_graph_finished_semaphore_ != VK_NULL_HANDLE) {
    vkDestroySemaphore(device, render_graph_finished_semaphore_, nullptr);
  }

  if (timeline_semaphore_ != VK_NULL_HANDLE) {
    vkDestroySemaphore(device, timeline_semaphore_, nullptr);
  }

  staging_command_buffers_unused().clear();
  staging_command_buffers_in_use().clear();
  host_buffer_regions_.clear();

  command_pool_ = VK_NULL_HANDLE;
  render_graph_finished_semaphore_ = VK_NULL_HANDLE;
  timeline_semaphore_ = VK_NULL_HANDLE;
}

VKStagingBuffer::CommandBufferDescriptor VKStagingBuffer::staging_command_buffer_descriptor_get()
{
  VKDevice &device = VKBackend::get().device;
  Vector<CommandBufferDescriptor> &command_buffers_unused = staging_command_buffers_unused();
  Vector<CommandBufferDescriptor> &command_buffers_in_use = staging_command_buffers_in_use();
  /* Check for completed command buffers that can be reused. */

  uint64_t current_timeline_value;
  vkGetSemaphoreCounterValue(device.vk_handle(), timeline_semaphore_, &current_timeline_value);

  if (command_buffers_unused.is_empty()) {
    for (int64_t i = 0; i < command_buffers_in_use.size(); i++) {
      if (current_timeline_value >= command_buffers_in_use[i].timeline_value)
      {
        command_buffers_unused.append(command_buffers_in_use[i]);
        command_buffers_in_use.remove_and_reorder(i);
        i--;
      }
    }
  }
  /* Create new command buffers when there are no left to be reused. */
  if (command_buffers_unused.is_empty()) {
    Array<VkCommandBuffer> command_buffers(2);
    VkCommandBufferAllocateInfo command_buffer_allocate_info{};
    command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_allocate_info.commandPool = command_pool_;
    command_buffer_allocate_info.commandBufferCount = 2;

    VkSemaphoreCreateInfo semaphore_create_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkAllocateCommandBuffers(
        device.vk_handle(), &command_buffer_allocate_info, command_buffers.data());

    for (int64_t i = 0; i < command_buffers.size(); i++) {
      CommandBufferDescriptor command_buffer_desc{};
      command_buffer_desc.command_buffer = command_buffers[i];
      vkCreateSemaphore(device.vk_handle(),
                        &semaphore_create_info,
                        nullptr,
                        &command_buffer_desc.binary_semaphore);
      command_buffers_unused.append(command_buffer_desc);
    }
  }
  return command_buffers_unused.pop_last();
}

VkDeviceSize VKStagingBuffer::update_host_buffer_regions()
{
  VKDevice &device = VKBackend::get().device;

  /* Out of bounds. Return to the start. */
  if (current_start_region_ + region_size_ > HOST_BUFFER_SIZE) {
    current_start_region_ = 0;
  }


  std::deque<HostBufferRegion>::iterator found_region = std::lower_bound(
      host_buffer_regions_.begin(),
      host_buffer_regions_.end(),
      current_start_region_,
      [](const HostBufferRegion &region, VkDeviceSize start) { return region.start < start; });

  VkDeviceSize new_region_start = current_start_region_;
  VkDeviceSize new_region_end = current_start_region_ + region_size_;
  uint64_t max_timeline_value = 0;

  if (found_region != host_buffer_regions_.begin()) {
    std::deque<HostBufferRegion>::iterator prev = std::prev(found_region);
    if (prev->start + prev->size > new_region_start) {
      found_region = prev;
    }
  }

  std::deque<HostBufferRegion>::iterator erase_start = found_region;
  while (found_region != host_buffer_regions_.end() && found_region->start < new_region_end)
  {
    max_timeline_value = std::max(max_timeline_value, found_region->timeline_value);
    ++found_region;
  }

  /* Erase the region that overlaps with the current region */
  if (erase_start != found_region) {
    VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                  nullptr,
                                  0,
                                  1,
                                  &timeline_semaphore_,
                                  &max_timeline_value};
    vkWaitSemaphores(device.vk_handle(), &wait_info, UINT64_MAX);

    host_buffer_regions_.erase(erase_start, found_region);
  }

  /* Push the current region to host_buffer_regions_ */
  std::deque<HostBufferRegion>::iterator insert_it = std::lower_bound(
      host_buffer_regions_.begin(),
      host_buffer_regions_.end(),
      new_region_start,
      [](const HostBufferRegion &region, VkDeviceSize start) { return region.start < start; });

  host_buffer_regions_.insert(insert_it, {new_region_start, region_size_, timeline_value_ + 1});

  VkDeviceSize offset = current_start_region_;

  current_start_region_ += region_size_;

  return offset;
}

void VKStagingBuffer::copy_to_device(const void *data)
{
  BLI_assert(direction_ == Direction::HostToDevice);
  BLI_assert(host_buffer_.is_allocated());

  VKDevice &device = VKBackend::get().device;
  VKContext &context = *VKContext::get();

  render_graph::VKBufferSynchronizationNode::CreateInfo buffer_synchronization = {};
  buffer_synchronization.vk_buffer = device_buffer_.vk_handle();
  buffer_synchronization.dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
  buffer_synchronization.dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;

  /** We must send the queue ownership transfer barrier because device_buffer_ is created with
   * VK_SHARING_MODE_EXCLUSIVE.
   *
   * https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-queue-transfers
   */
  if (is_generic_queue_needed_) {
    buffer_synchronization.src_queue_family = device.generic_queue_family_get();
    buffer_synchronization.dst_queue_family = device.transfer_queue_family_get();
  }
  
  context.render_graph().add_node(buffer_synchronization);

  VkPipelineStageFlags wait_dst_stage = device.supports_extension(
                                            VK_KHR_MAINTENANCE_8_EXTENSION_NAME) ?
                                            VK_PIPELINE_STAGE_TRANSFER_BIT :
                                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

  {
    VkSemaphore wait_semaphore = context.thread_data().wait_render_graph_semaphore_get_and_reset();

    context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                                   RenderGraphFlushFlags::RENEW_RENDER_GRAPH |
                                   RenderGraphFlushFlags::WAIT_FOR_SUBMISSION,
                               wait_dst_stage,
                               wait_semaphore,
                               render_graph_finished_semaphore_);
  }

  CommandBufferDescriptor staging_command_buffer_desc = staging_command_buffer_descriptor_get();
  VkCommandBuffer staging_command_buffer = staging_command_buffer_desc.command_buffer;
  VkSemaphore staging_buffer_finished_semaphore = staging_command_buffer_desc.binary_semaphore;

  vkResetCommandBuffer(staging_command_buffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);

  VkCommandBufferBeginInfo command_buffer_begin_info{};
  command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(staging_command_buffer, &command_buffer_begin_info);

  /* Acquire barrier */
  if (is_generic_queue_needed_) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_NONE;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = device.generic_queue_family_get();
    barrier.dstQueueFamilyIndex = device.transfer_queue_family_get();
    barrier.buffer = device_buffer_.vk_handle();
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyFlags dependency =
        VK_DEPENDENCY_BY_REGION_BIT |
        (device.supports_extension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME) ?
             VK_DEPENDENCY_QUEUE_FAMILY_OWNERSHIP_TRANSFER_USE_ALL_STAGES_BIT_KHR :
             0);
    vkCmdPipelineBarrier(staging_command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         dependency,
                         0,
                         nullptr,
                         1,
                         &barrier,
                         0,
                         nullptr);
  }

  VkDeviceSize offset = update_host_buffer_regions();

  host_buffer_.update_sub_immediately(offset, region_size_, static_cast<const uint8_t *>(data));

  VkBufferCopy buffer_copy{};
  buffer_copy.srcOffset = offset;
  buffer_copy.dstOffset = device_buffer_offset_;
  buffer_copy.size = region_size_;

  vkCmdCopyBuffer(staging_command_buffer,
                  host_buffer_.vk_handle(),
                  device_buffer_.vk_handle(),
                  1,
                  &buffer_copy);

  /* Release barrier */
  if (is_generic_queue_needed_) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = device.transfer_queue_family_get();
    barrier.dstQueueFamilyIndex = device.generic_queue_family_get();
    barrier.buffer = device_buffer_.vk_handle();
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyFlags dependency =
        VK_DEPENDENCY_BY_REGION_BIT |
        (device.supports_extension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME) ?
             VK_DEPENDENCY_QUEUE_FAMILY_OWNERSHIP_TRANSFER_USE_ALL_STAGES_BIT_KHR :
             0);
    vkCmdPipelineBarrier(staging_command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         dependency,
                         0,
                         nullptr,
                         1,
                         &barrier,
                         0,
                         nullptr);


    render_graph::VKBufferSynchronizationNode::CreateInfo buffer_synchronization = {};
    buffer_synchronization.vk_buffer = device_buffer_.vk_handle();
    buffer_synchronization.dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    buffer_synchronization.dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer_synchronization.src_queue_family = device.transfer_queue_family_get();
    buffer_synchronization.dst_queue_family = device.generic_queue_family_get();

    context.render_graph().add_node(buffer_synchronization);
  }

  vkEndCommandBuffer(staging_command_buffer);

  constexpr uint32_t signal_semaphores_len = 2;
  VkSemaphore signal_semaphores[2] = {timeline_semaphore_, staging_buffer_finished_semaphore};
  uint64_t signal_values[2] = {++timeline_value_, 0};

  VkTimelineSemaphoreSubmitInfo timeline_semaphore_submit_info{};
  timeline_semaphore_submit_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timeline_semaphore_submit_info.signalSemaphoreValueCount = signal_semaphores_len;
  timeline_semaphore_submit_info.pSignalSemaphoreValues = signal_values;

  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.pNext = &timeline_semaphore_submit_info;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &staging_command_buffer;
  submit_info.pWaitDstStageMask = &wait_dst_stage;
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = &render_graph_finished_semaphore_;
  submit_info.signalSemaphoreCount = signal_semaphores_len;
  submit_info.pSignalSemaphores = signal_semaphores;

  staging_command_buffer_desc.timeline_value = timeline_value_;

  {
    std::scoped_lock lock(*device.transfer_queue_mutex);
    if (vkQueueSubmit(transfer_queue_, 1, &submit_info, nullptr) != VK_SUCCESS) {
      CLOG_ERROR(&LOG,
                 "Unable to write from the host buffer to the device buffer "
                 "because submitting to the queue is failed.");
    }
  }
  staging_command_buffers_in_use().append(std::move(staging_command_buffer_desc));

  context.thread_data().wait_render_graph_semaphore_set(staging_buffer_finished_semaphore);
  context.thread_data().wait_stage = wait_dst_stage;
}

void VKStagingBuffer::copy_from_device()
{
  BLI_assert(direction_ == Direction::DeviceToHost);
  BLI_assert(host_buffer_.is_allocated());

  VKDevice &device = VKBackend::get().device;
  VKContext &context = *VKContext::get();

  render_graph::VKBufferSynchronizationNode::CreateInfo buffer_synchronization = {};
  buffer_synchronization.vk_buffer = device_buffer_.vk_handle();
  buffer_synchronization.dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  buffer_synchronization.dst_access = VK_ACCESS_TRANSFER_READ_BIT;

  /** We must send the queue ownership transfer barrier because device_buffer_ is created with
   * VK_SHARING_MODE_EXCLUSIVE
   *
   * https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-queue-transfers
   */
  if (is_generic_queue_needed_) {
    buffer_synchronization.src_queue_family = device.generic_queue_family_get();
    buffer_synchronization.dst_queue_family = device.transfer_queue_family_get();
  }

  context.render_graph().add_node(buffer_synchronization);

  VkPipelineStageFlags wait_dst_stage = device.supports_extension(
                                            VK_KHR_MAINTENANCE_8_EXTENSION_NAME) ?
                                            VK_PIPELINE_STAGE_TRANSFER_BIT :
                                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  {
    VkSemaphore wait_semaphore = context.thread_data().wait_render_graph_semaphore_get_and_reset();

    context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                                   RenderGraphFlushFlags::RENEW_RENDER_GRAPH |
                                   RenderGraphFlushFlags::WAIT_FOR_SUBMISSION,
                               wait_dst_stage,
                               wait_semaphore,
                               render_graph_finished_semaphore_);
  }

  CommandBufferDescriptor staging_command_buffer_desc = staging_command_buffer_descriptor_get();
  VkCommandBuffer staging_command_buffer = staging_command_buffer_desc.command_buffer;
  VkSemaphore staging_buffer_finished_semaphore = staging_command_buffer_desc.binary_semaphore;

  vkResetCommandBuffer(staging_command_buffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);

  VkCommandBufferBeginInfo command_buffer_begin_info{};
  command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(staging_command_buffer, &command_buffer_begin_info);

  /* Acquire barrier */
  if (is_generic_queue_needed_) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_NONE;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = device.generic_queue_family_get();
    barrier.dstQueueFamilyIndex = device.transfer_queue_family_get();
    barrier.buffer = device_buffer_.vk_handle();
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkDependencyFlags dependency =
        VK_DEPENDENCY_BY_REGION_BIT |
        (device.supports_extension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME) ?
             VK_DEPENDENCY_QUEUE_FAMILY_OWNERSHIP_TRANSFER_USE_ALL_STAGES_BIT_KHR :
             0);

    vkCmdPipelineBarrier(staging_command_buffer,
                         src_stage,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         dependency,
                         0,
                         nullptr,
                         1,
                         &barrier,
                         0,
                         nullptr);
  }

  local_start_region_ = update_host_buffer_regions();

  VkBufferCopy buffer_copy{};
  buffer_copy.srcOffset = device_buffer_offset_;
  buffer_copy.dstOffset = local_start_region_;
  buffer_copy.size = region_size_;

  vkCmdCopyBuffer(staging_command_buffer,
                  device_buffer_.vk_handle(),
                  host_buffer_.vk_handle(),
                  1,
                  &buffer_copy);

  /* Release barrier */
  if (is_generic_queue_needed_) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = device.transfer_queue_family_get();
    barrier.dstQueueFamilyIndex = device.generic_queue_family_get();
    barrier.buffer = device_buffer_.vk_handle();
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyFlags dependency =
        VK_DEPENDENCY_BY_REGION_BIT |
        (device.supports_extension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME) ?
             VK_DEPENDENCY_QUEUE_FAMILY_OWNERSHIP_TRANSFER_USE_ALL_STAGES_BIT_KHR :
             0);
    vkCmdPipelineBarrier(staging_command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         dependency,
                         0,
                         nullptr,
                         1,
                         &barrier,
                         0,
                         nullptr);

    render_graph::VKBufferSynchronizationNode::CreateInfo buffer_synchronization = {};
    buffer_synchronization.vk_buffer = device_buffer_.vk_handle();
    buffer_synchronization.dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    buffer_synchronization.dst_access = VK_ACCESS_TRANSFER_READ_BIT;
    buffer_synchronization.src_queue_family = device.transfer_queue_family_get();
    buffer_synchronization.dst_queue_family = device.generic_queue_family_get();

    context.render_graph().add_node(buffer_synchronization);
  }
  vkEndCommandBuffer(staging_command_buffer);

  constexpr uint32_t signal_semaphores_len = 2;
  VkSemaphore signal_semaphores[2] = {timeline_semaphore_, staging_buffer_finished_semaphore};
  uint64_t signal_values[2] = {++timeline_value_, 0};

  VkTimelineSemaphoreSubmitInfo timeline_semaphore_submit_info{};
  timeline_semaphore_submit_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timeline_semaphore_submit_info.signalSemaphoreValueCount = signal_semaphores_len;
  timeline_semaphore_submit_info.pSignalSemaphoreValues = signal_values;

  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.pNext = &timeline_semaphore_submit_info;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &staging_command_buffer;
  submit_info.pWaitDstStageMask = &wait_dst_stage;
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = &render_graph_finished_semaphore_;
  submit_info.signalSemaphoreCount = signal_semaphores_len;
  submit_info.pSignalSemaphores = signal_semaphores;

  staging_command_buffer_desc.timeline_value = timeline_value_;

  {
    std::scoped_lock lock(*device.transfer_queue_mutex);
    if (vkQueueSubmit(transfer_queue_, 1, &submit_info, nullptr) != VK_SUCCESS) {
      CLOG_ERROR(&LOG,
                 "Unable to write from the device buffer to the host buffer "
                 "because the submission to the queue failed.");
    }
  }
  
  staging_command_buffers_in_use().append(std::move(staging_command_buffer_desc));

  context.thread_data().wait_render_graph_semaphore_set(staging_buffer_finished_semaphore);
  context.thread_data().wait_stage = wait_dst_stage;
}

void VKStagingBuffer::read(void *data) const
{
  VKDevice &device = VKBackend::get().device;
  VkSemaphoreWaitInfo semaphore_wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                          nullptr,
                                          0,
                                          1,
                                          &timeline_semaphore_,
                                          &timeline_value_};

  vkWaitSemaphores(device.vk_handle(), &semaphore_wait_info, UINT64_MAX);

  memcpy(static_cast<uint8_t *>(data),
         static_cast<uint8_t *>(host_buffer_.mapped_memory_get()) + local_start_region_,
         region_size_);
}

}  // namespace blender::gpu
