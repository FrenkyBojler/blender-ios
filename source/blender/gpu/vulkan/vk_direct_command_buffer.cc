/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_direct_command_buffer.hh"

#include <algorithm>

#include "vk_backend.hh"
#include "vk_context.hh"
#include "vk_device.hh"
#include "vk_texture.hh"

#include "CLG_log.h"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.vulkan"};

VKDirectCommandBuffer::VKDirectCommandBuffer() {}

VKDirectCommandBuffer::~VKDirectCommandBuffer()
{
  if (vk_command_buffer_ != VK_NULL_HANDLE) {
    VKDevice &device = VKBackend::get().device;
    if (thread_data_ != nullptr) {
      vkFreeCommandBuffers(device.vk_handle(), thread_data_->command_pool, 1, &vk_command_buffer_);
    }
    vk_command_buffer_ = VK_NULL_HANDLE;
  }
}

void VKDirectCommandBuffer::begin(VkCommandBuffer vk_command_buffer, VKThreadData &thread_data)
{
  BLI_assert(vk_command_buffer_ == VK_NULL_HANDLE);
  vk_command_buffer_ = vk_command_buffer;
  thread_data_ = &thread_data;
  begin_recording();
}

TimelineValue VKDirectCommandBuffer::submit(VkSemaphore wait_semaphore,
                                            VkPipelineStageFlags wait_stage,
                                            VkSemaphore signal_semaphore,
                                            VkFence signal_fence)
{
  BLI_assert(vk_command_buffer_ != VK_NULL_HANDLE);
  VKDevice &device = VKBackend::get().device;

  end_recording();

  uint32_t wait_semaphore_len = 1;
  VkSemaphore wait_semaphores[2] = {device.vk_timeline_semaphore_, wait_semaphore};
  uint64_t wait_semaphore_values[2] = {0, 0};

  uint32_t signal_semaphore_len = 1;
  VkSemaphore signal_semaphores[2] = {device.vk_timeline_semaphore_, signal_semaphore};
  uint64_t signal_semaphore_values[2] = {0, 0};

  TimelineValue timeline;
  {
    std::scoped_lock lock(device.orphaned_data.mutex_get());
    timeline = ++device.timeline_value_;
    device.orphaned_data.timeline_ = timeline;
    wait_semaphore_values[0] = timeline - 1;
    signal_semaphore_values[0] = timeline;
  }

  VkPipelineStageFlags stage_flags[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, wait_stage};

  if (wait_semaphore != VK_NULL_HANDLE) {
    wait_semaphore_len = 2;
  }
  if (signal_semaphore != VK_NULL_HANDLE) {
    signal_semaphore_len = 2;
  }

  VkTimelineSemaphoreSubmitInfo timeline_info = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                 nullptr,
                                                 wait_semaphore_len,
                                                 wait_semaphore_values,
                                                 signal_semaphore_len,
                                                 signal_semaphore_values};

  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              &timeline_info,
                              wait_semaphore_len,
                              wait_semaphores,
                              stage_flags,
                              1,
                              &vk_command_buffer_,
                              signal_semaphore_len,
                              signal_semaphores};

  {
    std::scoped_lock lock_queue(*device.queue_mutex_);
    vkQueueSubmit(device.vk_queue_, 1, &submit_info, signal_fence);
  }

  last_submitted_timeline_ = timeline;
  vk_command_buffer_ = VK_NULL_HANDLE;
  thread_data_ = nullptr;
  return timeline;
}

TimelineValue VKDirectCommandBuffer::restore_and_submit(VKContext & /*context*/,
                                                        VkSemaphore wait_semaphore,
                                                        VkPipelineStageFlags wait_stage,
                                                        VkSemaphore signal_semaphore,
                                                        VkFence signal_fence)
{
  if (vk_command_buffer_ == VK_NULL_HANDLE) {
    return 0;
  }

  if (is_rendering_) {
    end_rendering();
  }

  /* TODO: restore dirty textures to their preferred layout.
   * This requires looking up VKTexture from VkImage to get preferred_layout_.
   * For now the barrier tracker state is sufficient. */

  TimelineValue result = submit(wait_semaphore, wait_stage, signal_semaphore, signal_fence);
  reset_tracking();
  return result;
}

void VKDirectCommandBuffer::reset_tracking()
{
  image_states_.clear();
  buffer_states_.clear();
  dirty_images_.clear();
  is_rendering_ = false;
}

void VKDirectCommandBuffer::barrier_image(VkImage image,
                                          VkImageLayout required_layout,
                                          VkAccessFlags required_access,
                                          VkPipelineStageFlags required_stages,
                                          VkImageAspectFlags aspect_mask)
{
  BLI_assert(vk_command_buffer_ != VK_NULL_HANDLE);
  auto *state = image_states_.lookup_ptr(image);
  bool needs_barrier = true;
  if (state && state->layout == required_layout &&
      (state->access & required_access) == required_access &&
      (state->stages & required_stages) == required_stages)
  {
    /* Only skip barrier for read-after-read with same layout. */
    constexpr VkAccessFlags write_flags = VK_ACCESS_SHADER_WRITE_BIT |
                                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT |
                                          VK_ACCESS_MEMORY_WRITE_BIT;
    needs_barrier = (state->access & write_flags) != 0 || (required_access & write_flags) != 0;
  }
  if (!needs_barrier) {
    return;
  }

  /* Cannot call pipeline barriers inside a dynamic rendering instance. The render pass
   * manages layouts and access implicitly. Only accumulate access/stage flags — do NOT
   * update tracked layout, since no real barrier was issued and VVL may have diverged
   * (e.g. via vkCmdBeginRendering setting COLOR_ATTACHMENT_OPTIMAL for attachments). */
  if (is_rendering_) {
    image_states_.add_overwrite(
        image,
        {state ? state->layout : VK_IMAGE_LAYOUT_UNDEFINED,
         required_access | (state ? state->access : VK_ACCESS_NONE),
         required_stages | (state ? state->stages : VK_PIPELINE_STAGE_NONE)});
    return;
  }

  VkImageLayout src_layout = state ? state->layout : VK_IMAGE_LAYOUT_UNDEFINED;
  VkAccessFlags src_access = state ? state->access : VK_ACCESS_NONE;
  VkPipelineStageFlags src_stages = state ? state->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

  /* If transitioning away from an attachment layout, the render pass's implicit storeOp at
   * end-of-render-pass writes to the image. Include the corresponding pipeline stages to
   * avoid WRITE-AFTER-WRITE hazards with subsequent barriers. */
  if (src_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
      src_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
      src_layout == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL)
  {
    src_stages |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    src_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  }
  if (src_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    src_stages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  }

  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = required_access;
  barrier.oldLayout = src_layout;
  barrier.newLayout = required_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect_mask;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

  vkCmdPipelineBarrier(
      vk_command_buffer_, src_stages, required_stages, 0, 0, nullptr, 0, nullptr, 1, &barrier);

  image_states_.add_overwrite(
      image,
      {required_layout,
       required_access | (state ? state->access : VK_ACCESS_NONE),
       required_stages | (state ? state->stages : VK_PIPELINE_STAGE_NONE)});
}

void VKDirectCommandBuffer::barrier_image_to_general(VkImage image,
                                                     VkAccessFlags required_access,
                                                     VkImageAspectFlags aspect_mask)
{
  BLI_assert(vk_command_buffer_ != VK_NULL_HANDLE);
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = required_access;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect_mask;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

  vkCmdPipelineBarrier(vk_command_buffer_,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0,
                       0,
                       nullptr,
                       0,
                       nullptr,
                       1,
                       &barrier);

  image_states_.add_overwrite(
      image, {VK_IMAGE_LAYOUT_GENERAL, required_access, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT});
}

void VKDirectCommandBuffer::barrier_buffer(VkBuffer buffer,
                                           VkAccessFlags required_access,
                                           VkPipelineStageFlags required_stages)
{
  BLI_assert(vk_command_buffer_ != VK_NULL_HANDLE);
  if (buffer == VK_NULL_HANDLE) {
    return;
  }
  auto *state = buffer_states_.lookup_ptr(buffer);
  constexpr VkAccessFlags write_flags = VK_ACCESS_SHADER_WRITE_BIT |
                                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT |
                                        VK_ACCESS_MEMORY_WRITE_BIT;
  bool needs_barrier = true;
  if (state && (state->access & required_access) == required_access &&
      (state->stages & required_stages) == required_stages)
  {
    /* Only skip barrier for read-after-read. Any write access on either side needs a barrier. */
    needs_barrier = (state->access & write_flags) != 0 || (required_access & write_flags) != 0;
  }
  if (!needs_barrier) {
    return;
  }

  /* Cannot call pipeline barriers inside a dynamic rendering instance. Just update tracked state.
   */
  if (is_rendering_) {
    buffer_states_.add_overwrite(
        buffer,
        {required_access | (state ? state->access : VK_ACCESS_NONE),
         required_stages | (state ? state->stages : VK_PIPELINE_STAGE_NONE)});
    return;
  }

  VkAccessFlags src_access = state ? state->access : VK_ACCESS_NONE;
  VkPipelineStageFlags src_stages = state ? state->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

  VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = required_access;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer;
  barrier.offset = 0;
  barrier.size = VK_WHOLE_SIZE;

  vkCmdPipelineBarrier(
      vk_command_buffer_, src_stages, required_stages, 0, 0, nullptr, 1, &barrier, 0, nullptr);

  buffer_states_.add_overwrite(
      buffer,
      {required_access | (state ? state->access : VK_ACCESS_NONE),
       required_stages | (state ? state->stages : VK_PIPELINE_STAGE_NONE)});
}

void VKDirectCommandBuffer::mark_image_dirty(VkImage image)
{
  dirty_images_.add(image);
}

void VKDirectCommandBuffer::generate_mipmaps(const VKUpdateMipmapsData &data)
{
  VkImageMemoryBarrier image_memory_barrier = {};
  image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  image_memory_barrier.pNext = nullptr;
  image_memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  image_memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  image_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_memory_barrier.image = data.vk_image;
  image_memory_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  image_memory_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  image_memory_barrier.subresourceRange.aspectMask = data.vk_image_aspect;
  image_memory_barrier.subresourceRange.baseArrayLayer = 0;
  image_memory_barrier.subresourceRange.layerCount = data.layer_count;
  image_memory_barrier.subresourceRange.baseMipLevel = 0;
  image_memory_barrier.subresourceRange.levelCount = 1;

  VkImageBlit image_blit = {};
  image_blit.srcSubresource.aspectMask = data.vk_image_aspect;
  image_blit.srcSubresource.layerCount = data.layer_count;
  image_blit.srcSubresource.mipLevel = 1;
  image_blit.dstSubresource.aspectMask = data.vk_image_aspect;
  image_blit.dstSubresource.layerCount = data.layer_count;
  image_blit.dstSubresource.mipLevel = 1;

  int3 dst_size = data.l0_size;
  for (int src_mipmap : IndexRange(data.mipmaps - 1)) {
    int dst_mipmap = src_mipmap + 1;
    int3 src_size = dst_size;
    dst_size = math::max(src_size / 2, int3(1));

    image_memory_barrier.subresourceRange.baseMipLevel = src_mipmap;
    pipeline_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_DEPENDENCY_BY_REGION_BIT,
                     0,
                     nullptr,
                     0,
                     nullptr,
                     1,
                     &image_memory_barrier);

    image_blit.srcSubresource.mipLevel = src_mipmap;
    image_blit.srcOffsets[1] = {src_size.x, src_size.y, src_size.z};
    image_blit.dstSubresource.mipLevel = dst_mipmap;
    image_blit.dstOffsets[1] = {dst_size.x, dst_size.y, dst_size.z};
    blit_image(data.vk_image,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               data.vk_image,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               1,
               &image_blit,
               VK_FILTER_LINEAR);
  }

  image_memory_barrier.subresourceRange.baseMipLevel = 0;
  image_memory_barrier.subresourceRange.levelCount = data.mipmaps - 1;
  image_memory_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  image_memory_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  pipeline_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_DEPENDENCY_BY_REGION_BIT,
                   0,
                   nullptr,
                   0,
                   nullptr,
                   1,
                   &image_memory_barrier);

  mark_image_dirty(data.vk_image);
}

/* -------------------------------------------------------------------- */
/** \name VKCommandBufferInterface implementation
 * \{ */

void VKDirectCommandBuffer::begin_recording()
{
  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(vk_command_buffer_, &begin_info);
}

void VKDirectCommandBuffer::end_recording()
{
  vkEndCommandBuffer(vk_command_buffer_);
}

void VKDirectCommandBuffer::bind_pipeline(VkPipelineBindPoint pipeline_bind_point,
                                          VkPipeline pipeline)
{
  vkCmdBindPipeline(vk_command_buffer_, pipeline_bind_point, pipeline);
}

void VKDirectCommandBuffer::bind_descriptor_sets(VkPipelineBindPoint pipeline_bind_point,
                                                 VkPipelineLayout layout,
                                                 uint32_t first_set,
                                                 uint32_t descriptor_set_count,
                                                 const VkDescriptorSet *p_descriptor_sets,
                                                 uint32_t dynamic_offset_count,
                                                 const uint32_t *p_dynamic_offsets)
{
  vkCmdBindDescriptorSets(vk_command_buffer_,
                          pipeline_bind_point,
                          layout,
                          first_set,
                          descriptor_set_count,
                          p_descriptor_sets,
                          dynamic_offset_count,
                          p_dynamic_offsets);
}

void VKDirectCommandBuffer::bind_index_buffer(VkBuffer buffer,
                                              VkDeviceSize offset,
                                              VkIndexType index_type)
{
  const VKDevice &device = VKBackend::get().device;
  auto *state = buffer_states_.lookup_ptr(buffer);
  bool needs_barrier = (state == nullptr) || ((state->access & VK_ACCESS_INDEX_READ_BIT) == 0);
  if (is_rendering_ && needs_barrier) {
    device.functions.vkCmdEndRendering(vk_command_buffer_);
    is_rendering_ = false;
    barrier_buffer(buffer, VK_ACCESS_INDEX_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    device.functions.vkCmdBeginRendering(vk_command_buffer_, &stored_rendering_info_);
    is_rendering_ = true;
  }
  else if (!is_rendering_ && needs_barrier) {
    barrier_buffer(buffer, VK_ACCESS_INDEX_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
  }
  vkCmdBindIndexBuffer(vk_command_buffer_, buffer, offset, index_type);
}

void VKDirectCommandBuffer::bind_vertex_buffers(uint32_t first_binding,
                                                uint32_t binding_count,
                                                const VkBuffer *p_buffers,
                                                const VkDeviceSize *p_offsets)
{
  const VKDevice &device = VKBackend::get().device;
  for (uint32_t i = 0; i < binding_count; i++) {
    if (p_buffers[i] == VK_NULL_HANDLE) {
      continue;
    }
    auto *state = buffer_states_.lookup_ptr(p_buffers[i]);
    bool needs_barrier = (state == nullptr) ||
                         ((state->access & VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT) == 0);
    if (is_rendering_ && needs_barrier) {
      device.functions.vkCmdEndRendering(vk_command_buffer_);
      is_rendering_ = false;
      barrier_buffer(
          p_buffers[i], VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
      device.functions.vkCmdBeginRendering(vk_command_buffer_, &stored_rendering_info_);
      is_rendering_ = true;
    }
    else if (!is_rendering_ && needs_barrier) {
      barrier_buffer(
          p_buffers[i], VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    }
  }
  vkCmdBindVertexBuffers(vk_command_buffer_, first_binding, binding_count, p_buffers, p_offsets);
}

void VKDirectCommandBuffer::draw(uint32_t vertex_count,
                                 uint32_t instance_count,
                                 uint32_t first_vertex,
                                 uint32_t first_instance)
{
  if (!is_rendering_) {
    const VKDevice &device = VKBackend::get().device;
    device.functions.vkCmdBeginRendering(vk_command_buffer_, &stored_rendering_info_);
    is_rendering_ = true;
  }
  vkCmdDraw(vk_command_buffer_, vertex_count, instance_count, first_vertex, first_instance);
}

void VKDirectCommandBuffer::draw_indexed(uint32_t index_count,
                                         uint32_t instance_count,
                                         uint32_t first_index,
                                         int32_t vertex_offset,
                                         uint32_t first_instance)
{
  if (!is_rendering_) {
    const VKDevice &device = VKBackend::get().device;
    device.functions.vkCmdBeginRendering(vk_command_buffer_, &stored_rendering_info_);
    is_rendering_ = true;
  }
  vkCmdDrawIndexed(
      vk_command_buffer_, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VKDirectCommandBuffer::draw_indirect(VkBuffer buffer,
                                          VkDeviceSize offset,
                                          uint32_t draw_count,
                                          uint32_t stride)
{
  if (buffer == VK_NULL_HANDLE) {
    return;
  }
  const VKDevice &device = VKBackend::get().device;
  if (is_rendering_) {
    device.functions.vkCmdEndRendering(vk_command_buffer_);
    is_rendering_ = false;
    barrier_buffer(
        buffer, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
    device.functions.vkCmdBeginRendering(vk_command_buffer_, &stored_rendering_info_);
    is_rendering_ = true;
  }
  else {
    barrier_buffer(
        buffer, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
  }
  vkCmdDrawIndirect(vk_command_buffer_, buffer, offset, draw_count, stride);
}

void VKDirectCommandBuffer::draw_indexed_indirect(VkBuffer buffer,
                                                  VkDeviceSize offset,
                                                  uint32_t draw_count,
                                                  uint32_t stride)
{
  if (buffer == VK_NULL_HANDLE) {
    return;
  }
  const VKDevice &device = VKBackend::get().device;
  if (is_rendering_) {
    device.functions.vkCmdEndRendering(vk_command_buffer_);
    is_rendering_ = false;
    barrier_buffer(
        buffer, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
    device.functions.vkCmdBeginRendering(vk_command_buffer_, &stored_rendering_info_);
    is_rendering_ = true;
  }
  else {
    barrier_buffer(
        buffer, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
  }
  vkCmdDrawIndexedIndirect(vk_command_buffer_, buffer, offset, draw_count, stride);
}

void VKDirectCommandBuffer::dispatch(uint32_t group_count_x,
                                     uint32_t group_count_y,
                                     uint32_t group_count_z)
{
  if (is_rendering_) {
    end_rendering();
  }
  vkCmdDispatch(vk_command_buffer_, group_count_x, group_count_y, group_count_z);
}

void VKDirectCommandBuffer::dispatch_indirect(VkBuffer buffer, VkDeviceSize offset)
{
  if (buffer == VK_NULL_HANDLE) {
    return;
  }
  if (is_rendering_) {
    end_rendering();
  }
  barrier_buffer(buffer, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
  vkCmdDispatchIndirect(vk_command_buffer_, buffer, offset);
}

void VKDirectCommandBuffer::update_buffer(VkBuffer dst_buffer,
                                          VkDeviceSize dst_offset,
                                          VkDeviceSize data_size,
                                          const void *p_data)
{
  if (is_rendering_) {
    end_rendering();
  }
  barrier_buffer(dst_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdUpdateBuffer(vk_command_buffer_, dst_buffer, dst_offset, data_size, p_data);
}

void VKDirectCommandBuffer::copy_buffer(VkBuffer src_buffer,
                                        VkBuffer dst_buffer,
                                        uint32_t region_count,
                                        const VkBufferCopy *p_regions)
{
  if (src_buffer == VK_NULL_HANDLE || dst_buffer == VK_NULL_HANDLE) {
    return;
  }
  if (is_rendering_) {
    end_rendering();
  }
  barrier_buffer(src_buffer, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  barrier_buffer(dst_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdCopyBuffer(vk_command_buffer_, src_buffer, dst_buffer, region_count, p_regions);
}

void VKDirectCommandBuffer::copy_image(VkImage src_image,
                                       VkImageLayout src_image_layout,
                                       VkImage dst_image,
                                       VkImageLayout dst_image_layout,
                                       uint32_t region_count,
                                       const VkImageCopy *p_regions,
                                       VkImageAspectFlags src_aspect_mask,
                                       VkImageAspectFlags dst_aspect_mask)
{
  if (is_rendering_) {
    end_rendering();
  }
  barrier_image(src_image,
                src_image_layout,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                src_aspect_mask);
  barrier_image(dst_image,
                dst_image_layout,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                dst_aspect_mask);
  vkCmdCopyImage(vk_command_buffer_,
                 src_image,
                 src_image_layout,
                 dst_image,
                 dst_image_layout,
                 region_count,
                 p_regions);
}

void VKDirectCommandBuffer::blit_image(VkImage src_image,
                                       VkImageLayout src_image_layout,
                                       VkImage dst_image,
                                       VkImageLayout dst_image_layout,
                                       uint32_t region_count,
                                       const VkImageBlit *p_regions,
                                       VkFilter filter,
                                       VkImageAspectFlags src_aspect_mask,
                                       VkImageAspectFlags dst_aspect_mask)
{
  if (is_rendering_) {
    end_rendering();
  }
  barrier_image(src_image,
                src_image_layout,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                src_aspect_mask);
  barrier_image(dst_image,
                dst_image_layout,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                dst_aspect_mask);
  vkCmdBlitImage(vk_command_buffer_,
                 src_image,
                 src_image_layout,
                 dst_image,
                 dst_image_layout,
                 region_count,
                 p_regions,
                 filter);
}

void VKDirectCommandBuffer::copy_buffer_to_image(VkBuffer src_buffer,
                                                 VkImage dst_image,
                                                 VkImageLayout dst_image_layout,
                                                 uint32_t region_count,
                                                 const VkBufferImageCopy *p_regions,
                                                 VkImageAspectFlags dst_aspect_mask)
{
  if (is_rendering_) {
    end_rendering();
  }
  barrier_buffer(src_buffer, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  barrier_image(dst_image,
                dst_image_layout,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                dst_aspect_mask);
  vkCmdCopyBufferToImage(
      vk_command_buffer_, src_buffer, dst_image, dst_image_layout, region_count, p_regions);
}

void VKDirectCommandBuffer::copy_image_to_buffer(VkImage src_image,
                                                 VkImageLayout src_image_layout,
                                                 VkBuffer dst_buffer,
                                                 uint32_t region_count,
                                                 const VkBufferImageCopy *p_regions,
                                                 VkImageAspectFlags src_aspect_mask)
{
  if (is_rendering_) {
    end_rendering();
  }
  barrier_image(src_image,
                src_image_layout,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                src_aspect_mask);
  barrier_buffer(dst_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdCopyImageToBuffer(
      vk_command_buffer_, src_image, src_image_layout, dst_buffer, region_count, p_regions);
}

void VKDirectCommandBuffer::fill_buffer(VkBuffer dst_buffer,
                                        VkDeviceSize dst_offset,
                                        VkDeviceSize size,
                                        uint32_t data)
{
  if (is_rendering_) {
    end_rendering();
  }
  barrier_buffer(dst_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  vkCmdFillBuffer(vk_command_buffer_, dst_buffer, dst_offset, size, data);
}

void VKDirectCommandBuffer::clear_color_image(VkImage image,
                                              VkImageLayout image_layout,
                                              const VkClearColorValue *p_color,
                                              uint32_t range_count,
                                              const VkImageSubresourceRange *p_ranges,
                                              VkImageAspectFlags aspect_mask)
{
  if (is_rendering_) {
    end_rendering();
  }
  barrier_image(image,
                image_layout,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                aspect_mask);
  vkCmdClearColorImage(vk_command_buffer_, image, image_layout, p_color, range_count, p_ranges);
}

void VKDirectCommandBuffer::clear_depth_stencil_image(
    VkImage image,
    VkImageLayout image_layout,
    const VkClearDepthStencilValue *p_depth_stencil,
    uint32_t range_count,
    const VkImageSubresourceRange *p_ranges,
    VkImageAspectFlags aspect_mask)
{
  if (is_rendering_) {
    end_rendering();
  }
  barrier_image(image,
                image_layout,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                aspect_mask);
  vkCmdClearDepthStencilImage(
      vk_command_buffer_, image, image_layout, p_depth_stencil, range_count, p_ranges);
}

void VKDirectCommandBuffer::clear_attachments(uint32_t attachment_count,
                                              const VkClearAttachment *p_attachments,
                                              uint32_t rect_count,
                                              const VkClearRect *p_rects)
{
  if (!is_rendering_) {
    const VKDevice &device = VKBackend::get().device;
    device.functions.vkCmdBeginRendering(vk_command_buffer_, &stored_rendering_info_);
    is_rendering_ = true;
  }
  vkCmdClearAttachments(vk_command_buffer_, attachment_count, p_attachments, rect_count, p_rects);
}

void VKDirectCommandBuffer::pipeline_barrier(VkPipelineStageFlags src_stage_mask,
                                             VkPipelineStageFlags dst_stage_mask,
                                             VkDependencyFlags dependency_flags,
                                             uint32_t memory_barrier_count,
                                             const VkMemoryBarrier *p_memory_barriers,
                                             uint32_t buffer_memory_barrier_count,
                                             const VkBufferMemoryBarrier *p_buffer_memory_barriers,
                                             uint32_t image_memory_barrier_count,
                                             const VkImageMemoryBarrier *p_image_memory_barriers)
{
  if (is_rendering_) {
    end_rendering();
  }
  vkCmdPipelineBarrier(vk_command_buffer_,
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

void VKDirectCommandBuffer::push_constants(VkPipelineLayout layout,
                                           VkShaderStageFlags stage_flags,
                                           uint32_t offset,
                                           uint32_t size,
                                           const void *p_values)
{
  vkCmdPushConstants(vk_command_buffer_, layout, stage_flags, offset, size, p_values);
}

void VKDirectCommandBuffer::set_viewport(const Vector<VkViewport> viewports)
{
  vkCmdSetViewport(vk_command_buffer_, 0, viewports.size(), viewports.data());
}

void VKDirectCommandBuffer::set_scissor(const Vector<VkRect2D> scissors)
{
  vkCmdSetScissor(vk_command_buffer_, 0, scissors.size(), scissors.data());
}

void VKDirectCommandBuffer::set_line_width(const float line_width)
{
  vkCmdSetLineWidth(vk_command_buffer_, line_width);
}

void VKDirectCommandBuffer::set_front_face(const VkFrontFace front_face)
{
  const VKDevice &device = VKBackend::get().device;
  BLI_assert(device.functions.vkCmdSetFrontFace);
  device.functions.vkCmdSetFrontFace(vk_command_buffer_, front_face);
}

void VKDirectCommandBuffer::set_vertex_input(
    Span<VkVertexInputBindingDescription2EXT> vertex_binding_descriptions,
    Span<VkVertexInputAttributeDescription2EXT> vertex_attribute_descriptions)
{
  const VKDevice &device = VKBackend::get().device;
  BLI_assert(device.functions.vkCmdSetVertexInput);
  device.functions.vkCmdSetVertexInput(vk_command_buffer_,
                                       vertex_binding_descriptions.size(),
                                       vertex_binding_descriptions.data(),
                                       vertex_attribute_descriptions.size(),
                                       vertex_attribute_descriptions.data());
}

void VKDirectCommandBuffer::set_stencil_compare_mask(const uint32_t compare_mask)
{
  vkCmdSetStencilCompareMask(vk_command_buffer_, VK_STENCIL_FACE_FRONT_AND_BACK, compare_mask);
}

void VKDirectCommandBuffer::set_stencil_write_mask(const uint32_t write_mask)
{
  vkCmdSetStencilWriteMask(vk_command_buffer_, VK_STENCIL_FACE_FRONT_AND_BACK, write_mask);
}

void VKDirectCommandBuffer::set_stencil_reference(const uint32_t reference)
{
  vkCmdSetStencilReference(vk_command_buffer_, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
}

void VKDirectCommandBuffer::begin_query(VkQueryPool vk_query_pool,
                                        uint32_t query_index,
                                        VkQueryControlFlags vk_query_control_flags)
{
  vkCmdBeginQuery(vk_command_buffer_, vk_query_pool, query_index, vk_query_control_flags);
}

void VKDirectCommandBuffer::end_query(VkQueryPool vk_query_pool, uint32_t query_index)
{
  vkCmdEndQuery(vk_command_buffer_, vk_query_pool, query_index);
}

void VKDirectCommandBuffer::reset_query_pool(VkQueryPool vk_query_pool,
                                             uint32_t first_query,
                                             uint32_t query_count)
{
  vkCmdResetQueryPool(vk_command_buffer_, vk_query_pool, first_query, query_count);
}

void VKDirectCommandBuffer::begin_rendering(const VkRenderingInfo *p_rendering_info)
{
  BLI_assert(!is_rendering_);
  is_rendering_ = true;
  const VKDevice &device = VKBackend::get().device;
  BLI_assert(device.functions.vkCmdBeginRendering);

  stored_rendering_info_ = *p_rendering_info;
  uint32_t color_count = std::min(p_rendering_info->colorAttachmentCount,
                                  uint32_t(MAX_COLOR_ATTACHMENTS));
  for (uint32_t i = 0; i < color_count; i++) {
    stored_color_attachments_[i] = p_rendering_info->pColorAttachments[i];
  }
  stored_rendering_info_.pColorAttachments = stored_color_attachments_;
  if (p_rendering_info->pDepthAttachment) {
    stored_depth_attachment_ = *p_rendering_info->pDepthAttachment;
    stored_rendering_info_.pDepthAttachment = &stored_depth_attachment_;
  }
  else {
    stored_rendering_info_.pDepthAttachment = nullptr;
  }
  if (p_rendering_info->pStencilAttachment) {
    stored_stencil_attachment_ = *p_rendering_info->pStencilAttachment;
    stored_rendering_info_.pStencilAttachment = &stored_stencil_attachment_;
  }
  else {
    stored_rendering_info_.pStencilAttachment = nullptr;
  }

  device.functions.vkCmdBeginRendering(vk_command_buffer_, &stored_rendering_info_);
}

void VKDirectCommandBuffer::end_rendering()
{
  BLI_assert(is_rendering_);
  is_rendering_ = false;
  const VKDevice &device = VKBackend::get().device;
  BLI_assert(device.functions.vkCmdEndRendering);
  device.functions.vkCmdEndRendering(vk_command_buffer_);
}

void VKDirectCommandBuffer::begin_debug_utils_label(
    const VkDebugUtilsLabelEXT *vk_debug_utils_label)
{
  VKDevice &device = VKBackend::get().device;
  if (device.functions.vkCmdBeginDebugUtilsLabel) {
    device.functions.vkCmdBeginDebugUtilsLabel(vk_command_buffer_, vk_debug_utils_label);
  }
}

void VKDirectCommandBuffer::end_debug_utils_label()
{
  VKDevice &device = VKBackend::get().device;
  if (device.functions.vkCmdEndDebugUtilsLabel) {
    device.functions.vkCmdEndDebugUtilsLabel(vk_command_buffer_);
  }
}

/** \} */

}  // namespace blender::gpu
