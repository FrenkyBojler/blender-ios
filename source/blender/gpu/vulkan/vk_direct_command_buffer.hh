/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "vk_command_buffer_interface.hh"
#include "vk_common.hh"

namespace blender::gpu {

class VKContext;
class VKThreadData;
class VKTexture;

/** Mipmap generation helper. */
struct VKUpdateMipmapsData {
  VkImage vk_image;
  VkImageAspectFlags vk_image_aspect;
  int mipmaps;
  int layer_count;
  int3 l0_size;
};

/**
 * Per-context command buffer for direct mode (without render graph).
 *
 * Records Vulkan commands directly into a VkCommandBuffer and tracks per-resource state
 * for automatic pipeline barrier insertion. On context deactivation, dirty textures
 * are transitioned back to their preferred layout.
 */
class VKDirectCommandBuffer : public render_graph::VKCommandBufferInterface {
  VkCommandBuffer vk_command_buffer_ = VK_NULL_HANDLE;
  VKThreadData *thread_data_ = nullptr;

 public:
  const VkCommandBuffer &vk_handle() const
  {
    return vk_command_buffer_;
  }

 private:

  /** Per-resource barrier tracking state. */
  struct ImageState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags access = VK_ACCESS_NONE;
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_NONE;
  };
  struct BufferState {
    VkAccessFlags access = VK_ACCESS_NONE;
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_NONE;
  };

  Map<VkImage, ImageState> image_states_;
  Map<VkBuffer, BufferState> buffer_states_;

  /** Textures whose layout was changed from their preferred layout. */
  Set<VkImage> dirty_images_;

  bool is_rendering_ = false;
  TimelineValue last_submitted_timeline_ = 0;

 public:
  VKDirectCommandBuffer();
  ~VKDirectCommandBuffer();

  /**
   * Begin recording on the given command buffer.
   */
  void begin(VkCommandBuffer vk_command_buffer, VKThreadData &thread_data);

  /**
   * Submit the current command buffer to the device queue.
   * Returns the timeline value for this submission.
   */
  TimelineValue submit(VkSemaphore wait_semaphore = VK_NULL_HANDLE,
                       VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_NONE,
                       VkSemaphore signal_semaphore = VK_NULL_HANDLE,
                       VkFence signal_fence = VK_NULL_HANDLE);

  /**
   * Transition all dirty textures back to their preferred layout, submit the
   * command buffer to the device queue, and clear tracking state.
   */
  TimelineValue restore_and_submit(VKContext &context,
                                   VkSemaphore wait_semaphore = VK_NULL_HANDLE,
                                   VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_NONE,
                                   VkSemaphore signal_semaphore = VK_NULL_HANDLE,
                                   VkFence signal_fence = VK_NULL_HANDLE);

  /** Clear all tracking state. */
  void reset_tracking();

  bool is_rendering() const
  {
    return is_rendering_;
  }

  TimelineValue last_submitted_timeline() const
  {
    return last_submitted_timeline_;
  }

  /**
   * Ensure that the given image is in the required layout/access/stage.
   * Inserts a pipeline barrier if the tracked state differs from the required state.
   */
  void barrier_image(VkImage image,
                     VkImageLayout required_layout,
                     VkAccessFlags required_access,
                     VkPipelineStageFlags required_stages,
                     VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT);

  /**
   * Ensure that the given buffer is in the required access/stage.
   * Inserts a pipeline barrier if the tracked state differs from the required state.
   */
  void barrier_buffer(VkBuffer buffer,
                      VkAccessFlags required_access,
                      VkPipelineStageFlags required_stages);

  /**
   * Generate mipmaps for the given image.
   * Calls vkCmdBlitImage for each mip level.
   */
  void generate_mipmaps(const VKUpdateMipmapsData &data);

  /** Mark an image as dirty (layout changed from preferred). */
  void mark_image_dirty(VkImage image);

  /* -------------------------------------------------------------------- */
  /** \name VKCommandBufferInterface implementation
   * \{ */

  void begin_recording() override;
  void end_recording() override;

  void bind_pipeline(VkPipelineBindPoint pipeline_bind_point, VkPipeline pipeline) override;
  void bind_descriptor_sets(VkPipelineBindPoint pipeline_bind_point,
                            VkPipelineLayout layout,
                            uint32_t first_set,
                            uint32_t descriptor_set_count,
                            const VkDescriptorSet *p_descriptor_sets,
                            uint32_t dynamic_offset_count,
                            const uint32_t *p_dynamic_offsets) override;
  void bind_index_buffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType index_type) override;
  void bind_vertex_buffers(uint32_t first_binding,
                           uint32_t binding_count,
                           const VkBuffer *p_buffers,
                           const VkDeviceSize *p_offsets) override;
  void draw(uint32_t vertex_count,
            uint32_t instance_count,
            uint32_t first_vertex,
            uint32_t first_instance) override;
  void draw_indexed(uint32_t index_count,
                    uint32_t instance_count,
                    uint32_t first_index,
                    int32_t vertex_offset,
                    uint32_t first_instance) override;
  void draw_indirect(VkBuffer buffer,
                     VkDeviceSize offset,
                     uint32_t draw_count,
                     uint32_t stride) override;
  void draw_indexed_indirect(VkBuffer buffer,
                             VkDeviceSize offset,
                             uint32_t draw_count,
                             uint32_t stride) override;
  void dispatch(uint32_t group_count_x,
                uint32_t group_count_y,
                uint32_t group_count_z) override;
  void dispatch_indirect(VkBuffer buffer, VkDeviceSize offset) override;
  void update_buffer(VkBuffer dst_buffer,
                     VkDeviceSize dst_offset,
                     VkDeviceSize data_size,
                     const void *p_data) override;
  void copy_buffer(VkBuffer src_buffer,
                   VkBuffer dst_buffer,
                   uint32_t region_count,
                   const VkBufferCopy *p_regions) override;
  void copy_image(VkImage src_image,
                  VkImageLayout src_image_layout,
                  VkImage dst_image,
                  VkImageLayout dst_image_layout,
                  uint32_t region_count,
                  const VkImageCopy *p_regions) override;
  void blit_image(VkImage src_image,
                  VkImageLayout src_image_layout,
                  VkImage dst_image,
                  VkImageLayout dst_image_layout,
                  uint32_t region_count,
                  const VkImageBlit *p_regions,
                  VkFilter filter) override;
  void copy_buffer_to_image(VkBuffer src_buffer,
                            VkImage dst_image,
                            VkImageLayout dst_image_layout,
                            uint32_t region_count,
                            const VkBufferImageCopy *p_regions) override;
  void copy_image_to_buffer(VkImage src_image,
                            VkImageLayout src_image_layout,
                            VkBuffer dst_buffer,
                            uint32_t region_count,
                            const VkBufferImageCopy *p_regions) override;
  void fill_buffer(VkBuffer dst_buffer,
                   VkDeviceSize dst_offset,
                   VkDeviceSize size,
                   uint32_t data) override;
  void clear_color_image(VkImage image,
                         VkImageLayout image_layout,
                         const VkClearColorValue *p_color,
                         uint32_t range_count,
                         const VkImageSubresourceRange *p_ranges) override;
  void clear_depth_stencil_image(VkImage image,
                                 VkImageLayout image_layout,
                                 const VkClearDepthStencilValue *p_depth_stencil,
                                 uint32_t range_count,
                                 const VkImageSubresourceRange *p_ranges) override;
  void clear_attachments(uint32_t attachment_count,
                         const VkClearAttachment *p_attachments,
                         uint32_t rect_count,
                         const VkClearRect *p_rects) override;
  void pipeline_barrier(VkPipelineStageFlags src_stage_mask,
                        VkPipelineStageFlags dst_stage_mask,
                        VkDependencyFlags dependency_flags,
                        uint32_t memory_barrier_count,
                        const VkMemoryBarrier *p_memory_barriers,
                        uint32_t buffer_memory_barrier_count,
                        const VkBufferMemoryBarrier *p_buffer_memory_barriers,
                        uint32_t image_memory_barrier_count,
                        const VkImageMemoryBarrier *p_image_memory_barriers) override;
  void push_constants(VkPipelineLayout layout,
                      VkShaderStageFlags stage_flags,
                      uint32_t offset,
                      uint32_t size,
                      const void *p_values) override;
  void set_viewport(const Vector<VkViewport> viewports) override;
  void set_scissor(const Vector<VkRect2D> scissors) override;
  void set_line_width(const float line_width) override;
  void set_front_face(const VkFrontFace front_face) override;
  void set_vertex_input(
      Span<VkVertexInputBindingDescription2EXT> vertex_binding_descriptions,
      Span<VkVertexInputAttributeDescription2EXT> vertex_attribute_descriptions) override;

  void set_stencil_compare_mask(const uint32_t compare_mask) override;
  void set_stencil_write_mask(const uint32_t write_mask) override;
  void set_stencil_reference(const uint32_t reference) override;
  void begin_query(VkQueryPool vk_query_pool,
                   uint32_t query_index,
                   VkQueryControlFlags vk_query_control_flags) override;
  void end_query(VkQueryPool vk_query_pool, uint32_t query_index) override;
  void reset_query_pool(VkQueryPool, uint32_t first_query, uint32_t query_count) override;
  void begin_rendering(const VkRenderingInfo *p_rendering_info) override;
  void end_rendering() override;
  void begin_debug_utils_label(const VkDebugUtilsLabelEXT *vk_debug_utils_label) override;
  void end_debug_utils_label() override;

  /** \} */
};

}  // namespace blender::gpu
