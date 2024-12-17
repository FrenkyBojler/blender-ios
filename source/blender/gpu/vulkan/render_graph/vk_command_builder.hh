/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_set.hh"

#include "vk_common.hh"
#include "vk_render_graph_node.hh"
#include "vk_scheduler.hh"

namespace blender::gpu::render_graph {
class VKRenderGraph;

struct LayeredImageBinding {
  VkImage vk_image;
  VkImageLayout vk_image_layout;
  uint32_t layer;
  uint32_t layer_count;
};

/**
 * Build the command buffer for sending to the device queue.
 *
 * Determine which nodes needs to be scheduled, Then for each node generate the needed pipeline
 * barriers and commands.
 */
class VKCommandBuilder {
  struct Barrier {
    IndexRange buffer_memory_barriers;
    IndexRange image_memory_barriers;

    VkPipelineStageFlags src_stage_mask = VK_PIPELINE_STAGE_NONE;
    VkPipelineStageFlags dst_stage_mask = VK_PIPELINE_STAGE_NONE;

    bool is_empty()
    {
      return buffer_memory_barriers.is_empty() && image_memory_barriers.is_empty();
    }
  };
  struct GroupNodeBarriers {

    /** Barriers to record before the group commands are recorded. */
    Barrier pre_barrier;

    /**
     * Barriers to record after the group commands are recorded.
     *
     * Post barriers are mainly used
     * to ensure each image resource is in a single known image layout. When attaching a
     * sub-resource (layers/mipmaps) a resource can have multiple layouts.
     */
    Barrier post_barrier;
  };

  /**
   * Range of indices that refers to the node groups that are part of the sub builder.
   */
  using SubBuilder = IndexRange;

  /**
   * Index range of the nodes of a group.
   *
   * The indexes are to `VKScheduler::result_` that is passed along `Span<NodeHandle> nodes` of
   * `build_nodes`.
   */
  using GroupNodes = IndexRange;

  /**
   * Index range into barrier_list_;
   */
  using Barriers = IndexRange;
  using BarrierIndex = int64_t;

 private:
  /* Pool of VKBufferMemoryBarriers that can be reused when building barriers */
  Vector<VkBufferMemoryBarrier> vk_buffer_memory_barriers_;
  Vector<VkImageMemoryBarrier> vk_image_memory_barriers_;

  /** Template buffer memory barrier. */
  VkBufferMemoryBarrier vk_buffer_memory_barrier_;
  /** Template image memory barrier. */
  VkImageMemoryBarrier vk_image_memory_barrier_;

  struct {
    /**
     * State of the bound pipelines during command building.
     */
    VKBoundPipelines active_pipelines;

    /**
     * Index of the active debug_group. Points to an element in
     * `VKRenderGraph.debug_.used_groups`.
     */
    int64_t active_debug_group_id = -1;
    /** Current level of debug groups. (number of nested debug groups). */
    int debug_level = 0;

    /**
     * All layered attachments of the last rendering scope (VKNodeType::BEGIN_RENDERING).
     *
     * when binding layer from these images we expect that they aren't used as attachment and can
     * be transitioned into a different image layout. These image layouts are stored in
     * `layered_bindings`.
     */
    Set<VkImage> layered_attachments;

    Vector<LayeredImageBinding> layered_bindings;
    const bool subresource_tracking_enabled() const
    {
      return !layered_attachments.is_empty();
    }
  } state_;

  /** Per sub builder store the index in the group_nodes_ and related other vectors. */
  Vector<SubBuilder> sub_builders_;
  /** Per group store the indices of the nodes. */
  Vector<GroupNodes> group_nodes_;
  /** Per group per node in group its pre execution barriers. */
  Vector<Barriers> group_pre_barriers_;

  /** List of all generated barriers. */
  Vector<Barrier> barrier_list_;

 public:
  VKCommandBuilder();

  /**
   * Build the commands of the nodes provided by the `node_handles` parameter. The commands are
   * recorded into the given `command_buffer`.
   *
   * Pre-condition:
   * - `command_buffer` must not be in initial state according to
   *   https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html#commandbuffers-lifecycle
   *
   * Post-condition:
   * - `command_buffer` will be in executable state according to
   *   https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html#commandbuffers-lifecycle
   */
  void build_nodes(VKRenderGraph &render_graph,
                   VKCommandBufferInterface &command_buffer,
                   Span<NodeHandle> node_handles);

 private:
  /**
   * Split the node_handles in logical groups.
   *
   * A new group is created when the next node is switching from data/compute to graphics and each
   * data/compute is also put in its own group.
   */

  void groups_init(const VKRenderGraph &render_graph, Span<NodeHandle> node_handles);

  /**
   * Extract the memory/buffer/image barriers from the commadn groups and add them to the pre/post
   * barriers.
   *
   * This process is single threaded as resource states change during the extraction process. The
   * result of this function would allow the sub builders to be built in parallel.
   */
  void groups_extract_barriers(VKRenderGraph &render_graph, Span<NodeHandle> node_handles);

  /**
   * Create sub builders for the given node_handles.
   *
   * Currently will only create a single sub_builder but will eventually split node handles into
   * multiple SubBuffers so we can multi-thread the command building.
   */
  void sub_builders_init(Span<NodeHandle> node_handles);

  void sub_builders_build_commands(VKRenderGraph &render_graph,
                                   VKCommandBufferInterface &command_buffer,
                                   Span<NodeHandle> node_handles);
  /** Record the secondary command buffers from the sub builders to the primary command buffer. */
  void sub_builders_record_to_primary_command_buffer(VKCommandBufferInterface &command_buffer);

  /**
   * Build the commands of the node group provided by the `node_group` parameter. The commands
   * are recorded into the given `command_buffer`.
   *
   * build_nodes splits the given node_handles into groups. All synchronization events inside
   * the group will be pushed to the front or back of this group. This allows us to record
   * resource usage on node level, perform reordering and then invoke the synchronization
   * events outside rendering scopes.
   */
  void build_node_group(VKRenderGraph &render_graph,
                        VKCommandBufferInterface &command_buffer,
                        Span<NodeHandle> node_handles,
                        int64_t node_group_index,
                        std::optional<NodeHandle> &r_rendering_scope);

  /**
   * Build the pipeline barriers that should be recorded before any other commands of the node
   * group the given node is part of is being recorded.
   */
  void build_pipeline_barriers(VKRenderGraph &render_graph,
                               VKCommandBufferInterface &command_buffer,
                               NodeHandle node_handle,
                               VkPipelineStageFlags pipeline_stage);
  void build_pipeline_barriers(VKRenderGraph &render_graph,
                               NodeHandle node_handle,
                               VkPipelineStageFlags pipeline_stage,
                               Barrier &r_barrier);
  void reset_barriers(Barrier &r_barrier);
  void send_pipeline_barriers(VKCommandBufferInterface &command_buffer, const Barrier &barrier);

  void add_buffer_barriers(VKRenderGraph &render_graph,
                           NodeHandle node_handle,
                           VkPipelineStageFlags node_stages,
                           Barrier &r_barrier);
  void add_buffer_barrier(VkBuffer vk_buffer,
                          VkAccessFlags src_access_mask,
                          VkAccessFlags dst_access_mask);
  void add_buffer_read_barriers(VKRenderGraph &render_graph,
                                NodeHandle node_handle,
                                VkPipelineStageFlags node_stages,
                                Barrier &r_barrier);
  void add_buffer_write_barriers(VKRenderGraph &render_graph,
                                 NodeHandle node_handle,
                                 VkPipelineStageFlags node_stages,
                                 Barrier &r_barrier);

  void add_image_barriers(VKRenderGraph &render_graph,
                          NodeHandle node_handle,
                          VkPipelineStageFlags node_stages,
                          Barrier &r_barrier);
  void add_image_barrier(VkImage vk_image,
                         Barrier &r_barrier,
                         VkAccessFlags src_access_mask,
                         VkAccessFlags dst_access_mask,
                         VkImageLayout old_image_layout,
                         VkImageLayout new_image_layout,
                         VkImageAspectFlags aspect_mask,
                         uint32_t layer_base = 0,
                         uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS);
  void add_image_read_barriers(VKRenderGraph &render_graph,
                               NodeHandle node_handle,
                               VkPipelineStageFlags node_stages,
                               Barrier &r_barrier);
  void add_image_write_barriers(VKRenderGraph &render_graph,
                                NodeHandle node_handle,
                                VkPipelineStageFlags node_stages,
                                Barrier &r_barrier);

  /**
   * Ensure that the debug group associated with the given node_handle is activated.
   *
   * When activating it determines how to walk from the current debug group to the to be activated
   * debug group by performing end/begin commands on the command buffer.
   *
   * This ensures that when nodes are reordered that they still appear in the right debug group.
   */
  void activate_debug_group(VKRenderGraph &render_graph,
                            VKCommandBufferInterface &command_buffer,
                            NodeHandle node_handle);

  /**
   * Make sure no debugging groups are active anymore.
   */
  void finish_debug_groups(VKCommandBufferInterface &command_buffer);

 private:
  /**
   * Update the layered attachments list when beginning a new render scope.
   */
  void layer_tracking_begin(const VKRenderGraph &render_graph, NodeHandle node_handle);

  /**
   * Ensure the layout of a layer.
   *
   * - `old_layout` should be the expected layout of the full image.
   */
  void layer_tracking_update(VkImage vk_image,
                             uint32_t layer,
                             uint32_t layer_count,
                             VkImageLayout old_layout,
                             VkImageLayout new_layout,
                             Barrier &r_barrier);

  /**
   * End layer tracking.
   *
   * All modified layers (layer_tracking_update) will be changed back to the image layout of
   * the texture (most likely a `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`).
   *
   * Render suspension/resuming will not work after calling this method.
   */
  void layer_tracking_end(VKCommandBufferInterface &command_buffer);

  /**
   * Suspend layer tracking
   *
   * Temporarily suspend layer tracking. This transits all modified layers back to its original
   * layout.
   * NOTE: Only call this method when you the rendering will be resumed, otherwise use
   * `layer_tracking_end`.
   */
  void layer_tracking_suspend(VKCommandBufferInterface &command_buffer);

  /**
   * Resume suspended layer tracking.
   *
   * Resume suspended layer tracking. This transits all registered layers back to its modified
   * state.
   */
  void layer_tracking_resume(VKCommandBufferInterface &command_buffer);
};

}  // namespace blender::gpu::render_graph
