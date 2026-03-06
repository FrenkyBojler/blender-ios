/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <optional>

#include "vk_node_info.hh"

namespace blender::gpu::render_graph {

/**
 * Information stored inside the render graph node. See `VKRenderGraphNode`.
 */
struct VKBufferSynchronizationData {};

/**
 * Information needed to add a node to the render graph.
 */
struct VKBufferSynchronizationCreateInfo {
  VkBuffer vk_buffer;
  VkPipelineStageFlags dst_stage;
  VkAccessFlags dst_access;
  uint32_t src_queue_family = VK_QUEUE_FAMILY_IGNORED;
  uint32_t dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
};

class VKBufferSynchronizationNode : public VKNodeInfo<VKNodeType::BUFFER_SYNCHRONIZATION,
                                                      VKBufferSynchronizationCreateInfo,
                                                      VKBufferSynchronizationData,
                                                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                      VKResourceType::BUFFER> {
 public:
  /**
   * Update the node data with the data inside create_info.
   *
   * Has been implemented as a template to ensure all node specific data
   * (`VK*Data`/`VK*CreateInfo`) types can be included in the same header file as the logic. The
   * actual node data (`VKRenderGraphNode` includes all header files.)
   */
  template<typename Node, typename Storage>
  static void set_node_data(Node &node, Storage & /* storage */, const CreateInfo &create_info)
  {
    UNUSED_VARS(create_info);
    node.buffer_synchronization = {};
  }

  /**
   * Extract read/write resource dependencies from `create_info` and add them to `node_links`.
   */
  void build_links(VKResourceStateTracker &resources,
                   VKRenderGraphLinks &links,
                   const CreateInfo &create_info) override
  {
    pipeline_stage = create_info.dst_stage;
    constexpr VkAccessFlags buffer_write_accesses = VK_ACCESS_SHADER_WRITE_BIT |
                                                    VK_ACCESS_TRANSFER_WRITE_BIT |
                                                    VK_ACCESS_HOST_WRITE_BIT;
    constexpr VkAccessFlags buffer_read_accesses = VK_ACCESS_UNIFORM_READ_BIT |
                                                   VK_ACCESS_INDEX_READ_BIT |
                                                   VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                                   VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                                                   VK_ACCESS_SHADER_READ_BIT |
                                                   VK_ACCESS_TRANSFER_READ_BIT |
                                                   VK_ACCESS_HOST_READ_BIT;
    if (bool(create_info.dst_access & buffer_write_accesses)) {
      ResourceWithStamp resource = resources.get_buffer_and_increase_stamp(create_info.vk_buffer);
      links.buffers.append({resource,
                            create_info.dst_access,
                            create_info.src_queue_family,
                            create_info.dst_queue_family});
    }
    else if (bool(create_info.dst_access & buffer_read_accesses)) {
      ResourceWithStamp resource = resources.get_buffer(create_info.vk_buffer);
      links.buffers.append({resource,
                            create_info.dst_access,
                            create_info.src_queue_family,
                            create_info.dst_queue_family});
    }
  }

  /**
   * Build the commands and add them to the command_buffer.
   */
  void build_commands(VKCommandBufferInterface &command_buffer,
                      Data &data,
                      Span<uint8_t> /*storage_push_constants*/,
                      VKBoundPipelines &/*r_bound_pipelines*/) override
  {
    UNUSED_VARS(command_buffer, data);
    /* Intentionally left empty: A pipeline barrier has already been send to the command buffer.
     */
  }
};
}  // namespace blender::gpu::render_graph
