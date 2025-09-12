/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "render_graph/vk_resource_access_info.hh"
#include "vk_node_info.hh"

namespace blender::gpu::render_graph {

/**
 * Information stored inside the render graph node. See `VKRenderGraphNode`.
 */
struct VKBuildAccelerationStructureData {
  VkAccelerationStructureBuildGeometryInfoKHR vk_acceleration_structure_build_geometry_info;
  Vector<VkAccelerationStructureGeometryKHR> vk_acceleration_structure_geometries;
  Vector<VkAccelerationStructureBuildRangeInfoKHR> vk_acceleration_structure_build_range_infos;
};
struct VKBuildAccelerationStructureCreateInfo {
  VKBuildAccelerationStructureData node_data;
  Set<VkBuffer> src_buffers;
  VkBuffer dst_acceleration_structure;
};

class VKBuildAccelerationStructureNode
    : public VKNodeInfo<VKNodeType::BUILD_ACCELERATION_STRUCTURE,
                        VKBuildAccelerationStructureCreateInfo,
                        VKBuildAccelerationStructureData,
                        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
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
  static void set_node_data(Node &node, Storage &storage, const CreateInfo &create_info)
  {
    node.storage_index = storage.build_acceleration_structure.append_and_get_index(
        create_info.node_data);
  }

  /**
   * Extract read/write resource dependencies from `create_info` and add them to `node_links`.
   */
  void build_links(VKResourceStateTracker &resources,
                   VKRenderGraphNodeLinks &node_links,
                   const CreateInfo &create_info) override
  {
    for (VkBuffer vk_buffer : create_info.src_buffers) {
      BLI_assert(vk_buffer != VK_NULL_HANDLE);
      ResourceWithStamp src_buffer = resources.get_buffer(vk_buffer);
      node_links.inputs.append({src_buffer, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR});
    }

    ResourceWithStamp dst_acceleration_structure = resources.get_buffer_and_increase_stamp(
        create_info.dst_acceleration_structure);
    node_links.outputs.append(
        {dst_acceleration_structure, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR});
  }

  /**
   * Build the commands and add them to the command_buffer.
   */
  void build_commands(VKCommandBufferInterface &command_buffer,
                      Data &data,
                      VKBoundPipelines & /*r_bound_pipelines*/) override
  {
    data.vk_acceleration_structure_build_geometry_info.geometryCount = uint32_t(
        data.vk_acceleration_structure_geometries.size());
    data.vk_acceleration_structure_build_geometry_info.pGeometries =
        data.vk_acceleration_structure_geometries.data();

    command_buffer.build_acceleration_structure(
        &data.vk_acceleration_structure_build_geometry_info,
        data.vk_acceleration_structure_build_range_infos.data());
  }
};
}  // namespace blender::gpu::render_graph
