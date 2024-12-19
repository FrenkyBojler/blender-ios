/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_command_buffer_wrapper.hh"
#include "vk_backend.hh"
#include "vk_device.hh"

namespace blender::gpu::render_graph {
VKCommandBufferWrapper::VKCommandBufferWrapper(const VKWorkarounds &workarounds)
{
  vk_command_pool_create_info_ = {};
  vk_command_pool_create_info_.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  vk_command_pool_create_info_.queueFamilyIndex = 0;

  vk_command_buffer_begin_info_ = {};
  vk_command_buffer_begin_info_.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vk_command_buffer_begin_info_.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk_command_buffer_begin_info_.pInheritanceInfo = &vk_command_buffer_inheritance_info_;

  vk_command_buffer_inheritance_info_ = {};
  vk_command_buffer_inheritance_info_.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;

  vk_fence_create_info_ = {};
  vk_fence_create_info_.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  vk_fence_create_info_.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  vk_submit_info_ = {};
  vk_submit_info_.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  vk_submit_info_.waitSemaphoreCount = 0;
  vk_submit_info_.pWaitSemaphores = nullptr;
  vk_submit_info_.pWaitDstStageMask = nullptr;
  vk_submit_info_.commandBufferCount = 1;
  vk_submit_info_.pCommandBuffers = nullptr;
  vk_submit_info_.signalSemaphoreCount = 0;
  vk_submit_info_.pSignalSemaphores = nullptr;

  use_dynamic_rendering = !workarounds.dynamic_rendering;
}

VKCommandBufferWrapper::~VKCommandBufferWrapper()
{
  VKDevice &device = VKBackend::get().device;
  device.free_command_pool_buffers(vk_command_pool_);
  if (vk_command_pool_ != VK_NULL_HANDLE) {
    vkDestroyCommandPool(device.vk_handle(), vk_command_pool_, nullptr);
    vk_command_pool_ = VK_NULL_HANDLE;
  }
  if (vk_fence_ != VK_NULL_HANDLE) {
    vkDestroyFence(device.vk_handle(), vk_fence_, nullptr);
    vk_fence_ = VK_NULL_HANDLE;
  }
}

void VKCommandBufferWrapper::begin_recording(VkCommandBuffer vk_command_buffer)
{
  VKDevice &device = VKBackend::get().device;
  if (vk_fence_ == VK_NULL_HANDLE) {
    vkCreateFence(device.vk_handle(), &vk_fence_create_info_, nullptr, &vk_fence_);
  }

  vkBeginCommandBuffer(vk_command_buffer, &vk_command_buffer_begin_info_);
}

void VKCommandBufferWrapper::end_recording(VkCommandBuffer vk_command_buffer)
{
  vkEndCommandBuffer(vk_command_buffer);
}

void VKCommandBufferWrapper::submit_with_cpu_synchronization(VkCommandBuffer vk_command_buffer,
                                                             VkFence vk_fence)
{
  if (vk_fence == VK_NULL_HANDLE) {
    vk_fence = vk_fence_;
  }
  VKDevice &device = VKBackend::get().device;
  vkResetFences(device.vk_handle(), 1, &vk_fence);
  {
    std::scoped_lock lock(device.queue_mutex_get());
    vk_submit_info_.pCommandBuffers = &vk_command_buffer;
    vkQueueSubmit(device.queue_get(), 1, &vk_submit_info_, vk_fence);
  }

  /* Discard all command buffers that have been created since last submission. */
  for (VkCommandBuffer vk_command_buffer : command_buffers_) {
    device.discard_pool_for_current_thread(true).discard_command_buffer(vk_command_buffer,
                                                                        vk_command_pool_);
  }
  command_buffers_.clear();
}

void VKCommandBufferWrapper::wait_for_cpu_synchronization(VkFence vk_fence)
{
  if (vk_fence == VK_NULL_HANDLE) {
    vk_fence = vk_fence_;
  }
  VKDevice &device = VKBackend::get().device;
  while (vkWaitForFences(device.vk_handle(), 1, &vk_fence, true, UINT64_MAX) == VK_TIMEOUT) {
  }
}

void VKCommandBufferWrapper::bind_pipeline(VkCommandBuffer vk_command_buffer,
                                           VkPipelineBindPoint pipeline_bind_point,
                                           VkPipeline pipeline)
{
  vkCmdBindPipeline(vk_command_buffer, pipeline_bind_point, pipeline);
}

void VKCommandBufferWrapper::bind_descriptor_sets(VkCommandBuffer vk_command_buffer,
                                                  VkPipelineBindPoint pipeline_bind_point,
                                                  VkPipelineLayout layout,
                                                  uint32_t first_set,
                                                  uint32_t descriptor_set_count,
                                                  const VkDescriptorSet *p_descriptor_sets,
                                                  uint32_t dynamic_offset_count,
                                                  const uint32_t *p_dynamic_offsets)
{
  vkCmdBindDescriptorSets(vk_command_buffer,
                          pipeline_bind_point,
                          layout,
                          first_set,
                          descriptor_set_count,
                          p_descriptor_sets,
                          dynamic_offset_count,
                          p_dynamic_offsets);
}

void VKCommandBufferWrapper::bind_index_buffer(VkCommandBuffer vk_command_buffer,
                                               VkBuffer buffer,
                                               VkDeviceSize offset,
                                               VkIndexType index_type)
{
  vkCmdBindIndexBuffer(vk_command_buffer, buffer, offset, index_type);
}

void VKCommandBufferWrapper::bind_vertex_buffers(VkCommandBuffer vk_command_buffer,
                                                 uint32_t first_binding,
                                                 uint32_t binding_count,
                                                 const VkBuffer *p_buffers,
                                                 const VkDeviceSize *p_offsets)
{
  vkCmdBindVertexBuffers(vk_command_buffer, first_binding, binding_count, p_buffers, p_offsets);
}

void VKCommandBufferWrapper::draw(VkCommandBuffer vk_command_buffer,
                                  uint32_t vertex_count,
                                  uint32_t instance_count,
                                  uint32_t first_vertex,
                                  uint32_t first_instance)
{
  vkCmdDraw(vk_command_buffer, vertex_count, instance_count, first_vertex, first_instance);
}

void VKCommandBufferWrapper::draw_indexed(VkCommandBuffer vk_command_buffer,
                                          uint32_t index_count,
                                          uint32_t instance_count,
                                          uint32_t first_index,
                                          int32_t vertex_offset,
                                          uint32_t first_instance)
{
  vkCmdDrawIndexed(
      vk_command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VKCommandBufferWrapper::draw_indirect(VkCommandBuffer vk_command_buffer,
                                           VkBuffer buffer,
                                           VkDeviceSize offset,
                                           uint32_t draw_count,
                                           uint32_t stride)
{
  vkCmdDrawIndirect(vk_command_buffer, buffer, offset, draw_count, stride);
}

void VKCommandBufferWrapper::draw_indexed_indirect(VkCommandBuffer vk_command_buffer,
                                                   VkBuffer buffer,
                                                   VkDeviceSize offset,
                                                   uint32_t draw_count,
                                                   uint32_t stride)
{
  vkCmdDrawIndexedIndirect(vk_command_buffer, buffer, offset, draw_count, stride);
}

void VKCommandBufferWrapper::dispatch(VkCommandBuffer vk_command_buffer,
                                      uint32_t group_count_x,
                                      uint32_t group_count_y,
                                      uint32_t group_count_z)
{
  vkCmdDispatch(vk_command_buffer, group_count_x, group_count_y, group_count_z);
}

void VKCommandBufferWrapper::dispatch_indirect(VkCommandBuffer vk_command_buffer,
                                               VkBuffer buffer,
                                               VkDeviceSize offset)
{
  vkCmdDispatchIndirect(vk_command_buffer, buffer, offset);
}

void VKCommandBufferWrapper::update_buffer(VkCommandBuffer vk_command_buffer,
                                           VkBuffer dst_buffer,
                                           VkDeviceSize dst_offset,
                                           VkDeviceSize data_size,
                                           const void *p_data)
{
  vkCmdUpdateBuffer(vk_command_buffer, dst_buffer, dst_offset, data_size, p_data);
}

void VKCommandBufferWrapper::copy_buffer(VkCommandBuffer vk_command_buffer,
                                         VkBuffer src_buffer,
                                         VkBuffer dst_buffer,
                                         uint32_t region_count,
                                         const VkBufferCopy *p_regions)
{
  vkCmdCopyBuffer(vk_command_buffer, src_buffer, dst_buffer, region_count, p_regions);
}

void VKCommandBufferWrapper::copy_image(VkCommandBuffer vk_command_buffer,
                                        VkImage src_image,
                                        VkImageLayout src_image_layout,
                                        VkImage dst_image,
                                        VkImageLayout dst_image_layout,
                                        uint32_t region_count,
                                        const VkImageCopy *p_regions)
{
  vkCmdCopyImage(vk_command_buffer,
                 src_image,
                 src_image_layout,
                 dst_image,
                 dst_image_layout,
                 region_count,
                 p_regions);
}

void VKCommandBufferWrapper::blit_image(VkCommandBuffer vk_command_buffer,
                                        VkImage src_image,
                                        VkImageLayout src_image_layout,
                                        VkImage dst_image,
                                        VkImageLayout dst_image_layout,
                                        uint32_t region_count,
                                        const VkImageBlit *p_regions,
                                        VkFilter filter)
{
  vkCmdBlitImage(vk_command_buffer,
                 src_image,
                 src_image_layout,
                 dst_image,
                 dst_image_layout,
                 region_count,
                 p_regions,
                 filter);
}

void VKCommandBufferWrapper::copy_buffer_to_image(VkCommandBuffer vk_command_buffer,
                                                  VkBuffer src_buffer,
                                                  VkImage dst_image,
                                                  VkImageLayout dst_image_layout,
                                                  uint32_t region_count,
                                                  const VkBufferImageCopy *p_regions)
{
  vkCmdCopyBufferToImage(
      vk_command_buffer, src_buffer, dst_image, dst_image_layout, region_count, p_regions);
}

void VKCommandBufferWrapper::copy_image_to_buffer(VkCommandBuffer vk_command_buffer,
                                                  VkImage src_image,
                                                  VkImageLayout src_image_layout,
                                                  VkBuffer dst_buffer,
                                                  uint32_t region_count,
                                                  const VkBufferImageCopy *p_regions)
{
  vkCmdCopyImageToBuffer(
      vk_command_buffer, src_image, src_image_layout, dst_buffer, region_count, p_regions);
}

void VKCommandBufferWrapper::fill_buffer(VkCommandBuffer vk_command_buffer,
                                         VkBuffer dst_buffer,
                                         VkDeviceSize dst_offset,
                                         VkDeviceSize size,
                                         uint32_t data)
{
  vkCmdFillBuffer(vk_command_buffer, dst_buffer, dst_offset, size, data);
}

void VKCommandBufferWrapper::clear_color_image(VkCommandBuffer vk_command_buffer,
                                               VkImage image,
                                               VkImageLayout image_layout,
                                               const VkClearColorValue *p_color,
                                               uint32_t range_count,
                                               const VkImageSubresourceRange *p_ranges)
{
  vkCmdClearColorImage(vk_command_buffer, image, image_layout, p_color, range_count, p_ranges);
}
void VKCommandBufferWrapper::clear_depth_stencil_image(
    VkCommandBuffer vk_command_buffer,
    VkImage image,
    VkImageLayout image_layout,
    const VkClearDepthStencilValue *p_depth_stencil,
    uint32_t range_count,
    const VkImageSubresourceRange *p_ranges)
{
  vkCmdClearDepthStencilImage(
      vk_command_buffer, image, image_layout, p_depth_stencil, range_count, p_ranges);
}

void VKCommandBufferWrapper::clear_attachments(VkCommandBuffer vk_command_buffer,
                                               uint32_t attachment_count,
                                               const VkClearAttachment *p_attachments,
                                               uint32_t rect_count,
                                               const VkClearRect *p_rects)
{
  vkCmdClearAttachments(vk_command_buffer, attachment_count, p_attachments, rect_count, p_rects);
}

void VKCommandBufferWrapper::pipeline_barrier(
    VkCommandBuffer vk_command_buffer,
    VkPipelineStageFlags src_stage_mask,
    VkPipelineStageFlags dst_stage_mask,
    VkDependencyFlags dependency_flags,
    uint32_t memory_barrier_count,
    const VkMemoryBarrier *p_memory_barriers,
    uint32_t buffer_memory_barrier_count,
    const VkBufferMemoryBarrier *p_buffer_memory_barriers,
    uint32_t image_memory_barrier_count,
    const VkImageMemoryBarrier *p_image_memory_barriers)
{
  vkCmdPipelineBarrier(vk_command_buffer,
                       src_stage_mask,
                       dst_stage_mask,
                       dependency_flags,
                       memory_barrier_count,
                       p_memory_barriers,
                       buffer_memory_barrier_count,
                       p_buffer_memory_barriers,
                       image_memory_barrier_count,
                       p_image_memory_barriers);
}

void VKCommandBufferWrapper::push_constants(VkCommandBuffer vk_command_buffer,
                                            VkPipelineLayout layout,
                                            VkShaderStageFlags stage_flags,
                                            uint32_t offset,
                                            uint32_t size,
                                            const void *p_values)
{
  vkCmdPushConstants(vk_command_buffer, layout, stage_flags, offset, size, p_values);
}

void VKCommandBufferWrapper::begin_render_pass(VkCommandBuffer vk_command_buffer,
                                               const VkRenderPassBeginInfo *render_pass_begin_info)
{
  vkCmdBeginRenderPass(vk_command_buffer, render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void VKCommandBufferWrapper::end_render_pass(VkCommandBuffer vk_command_buffer)
{
  vkCmdEndRenderPass(vk_command_buffer);
}

void VKCommandBufferWrapper::begin_rendering(VkCommandBuffer vk_command_buffer,
                                             const VkRenderingInfo *p_rendering_info)
{
  const VKDevice &device = VKBackend::get().device;
  BLI_assert(device.functions.vkCmdBeginRendering);
  device.functions.vkCmdBeginRendering(vk_command_buffer, p_rendering_info);
}

void VKCommandBufferWrapper::end_rendering(VkCommandBuffer vk_command_buffer)
{
  const VKDevice &device = VKBackend::get().device;
  BLI_assert(device.functions.vkCmdEndRendering);
  device.functions.vkCmdEndRendering(vk_command_buffer);
}

void VKCommandBufferWrapper::begin_query(VkCommandBuffer vk_command_buffer,
                                         VkQueryPool vk_query_pool,
                                         uint32_t query_index,
                                         VkQueryControlFlags vk_query_control_flags)
{
  vkCmdBeginQuery(vk_command_buffer, vk_query_pool, query_index, vk_query_control_flags);
}

void VKCommandBufferWrapper::end_query(VkCommandBuffer vk_command_buffer,
                                       VkQueryPool vk_query_pool,
                                       uint32_t query_index)
{
  vkCmdEndQuery(vk_command_buffer, vk_query_pool, query_index);
}

void VKCommandBufferWrapper::reset_query_pool(VkCommandBuffer vk_command_buffer,
                                              VkQueryPool vk_query_pool,
                                              uint32_t first_query,
                                              uint32_t query_count)
{
  vkCmdResetQueryPool(vk_command_buffer, vk_query_pool, first_query, query_count);
}

VkCommandBuffer VKCommandBufferWrapper::allocate_primary_command_buffer()
{
  VKDevice &device = VKBackend::get().device;
  ensure_command_buffer_pool(device);
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;

  VkCommandBufferAllocateInfo command_buffer_allocation_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      nullptr,
      vk_command_pool_,
      VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      1};
  vkAllocateCommandBuffers(device.vk_handle(), &command_buffer_allocation_info, &command_buffer);

  command_buffers_.append(command_buffer);
  return command_buffer;
}

Span<VkCommandBuffer> VKCommandBufferWrapper::allocate_secondary_command_buffers(
    uint32_t command_buffer_count)
{
  VKDevice &device = VKBackend::get().device;
  ensure_command_buffer_pool(device);
  IndexRange range = IndexRange::from_begin_size(command_buffers_.size(), command_buffer_count);
  VkCommandBuffer *command_buffers = command_buffers_.end();
  command_buffers_.append_n_times(VK_NULL_HANDLE, command_buffer_count);

  VkCommandBufferAllocateInfo command_buffer_allocation_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      nullptr,
      vk_command_pool_,
      VK_COMMAND_BUFFER_LEVEL_SECONDARY,
      command_buffer_count};
  vkAllocateCommandBuffers(device.vk_handle(), &command_buffer_allocation_info, command_buffers);

  return command_buffers_.as_span().slice(range);
}

void VKCommandBufferWrapper::begin_debug_utils_label(
    VkCommandBuffer vk_command_buffer, const VkDebugUtilsLabelEXT *vk_debug_utils_label)
{
  const VKDevice &device = VKBackend::get().device;
  if (device.functions.vkCmdBeginDebugUtilsLabel) {
    device.functions.vkCmdBeginDebugUtilsLabel(vk_command_buffer, vk_debug_utils_label);
  }
}

void VKCommandBufferWrapper::end_debug_utils_label(VkCommandBuffer vk_command_buffer)
{
  const VKDevice &device = VKBackend::get().device;
  if (device.functions.vkCmdEndDebugUtilsLabel) {
    device.functions.vkCmdEndDebugUtilsLabel(vk_command_buffer);
  }
}

void VKCommandBufferWrapper::ensure_command_buffer_pool(VKDevice &device)
{
  if (vk_command_pool_ == VK_NULL_HANDLE) {
    vk_command_pool_create_info_.queueFamilyIndex = device.queue_family_get();
    vkCreateCommandPool(
        device.vk_handle(), &vk_command_pool_create_info_, nullptr, &vk_command_pool_);
    vk_command_pool_create_info_.queueFamilyIndex = 0;
  }
}

}  // namespace blender::gpu::render_graph
