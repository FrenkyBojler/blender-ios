/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Draw and dispatch commands are shader based and resources needs to be bound. The bound resources
 * are stored inside the state manager. Structures and functions inside this file improve code
 * re-usage when resources are part of the state manager.
 *
 * VKResourceAccessInfo: is a structure that can store the access information of a draw/dispatch
 * node. This information should be added to the create info of the render graph node
 * (`VKNodeInfo::CreateInfo`). When the links of the node is build,
 * `VKResourceAccessInfo.build_links` can be called to build the render graph links for these
 * resources.
 */

#pragma once

#include "BLI_utility_mixins.hh"

#include "vk_common.hh"
#include "vk_render_graph_links.hh"

namespace blender::gpu::render_graph {
class VKResourceStateTracker;

/**
 * Convert shader stage flags to pipeline stage flags.
 *
 * Shader stages (VK_SHADER_STAGE_*) and pipeline stages (VK_PIPELINE_STAGE_*) are distinct
 * flag types but there is a direct mapping between shader stage bits and the corresponding
 * pipeline stage bits (e.g. VK_SHADER_STAGE_VERTEX_BIT -> VK_PIPELINE_STAGE_VERTEX_SHADER_BIT).
 */
VkPipelineStageFlags to_vk_pipeline_stage(VkShaderStageFlags shader_stages);

/** Struct describing the access to an image. */
struct VKImageAccess {
  VkImage vk_image;
  VkAccessFlags vk_access_flags;
  VkImageAspectFlags vk_image_aspect;
  /* Used for sub-resource tracking within a rendering scope.
   *
   * By default all layers of images are tracked as a single resource. Only inside a render scope
   * we can temporary change a subset of layers, when the image is used as an attachment and a
   * image load/store.
   */
  VKSubImageRange subimage;

  /** Determine the image layout for the vk_access_flags. */
  VkImageLayout to_vk_image_layout(bool supports_local_read) const;

  /**
   * Which pipeline stage(s) access the resource.
   *
   * When VK_PIPELINE_STAGE_NONE the node-level pipeline stage will be used as fallback.
   */
  VkPipelineStageFlags vk_pipeline_stages = VK_PIPELINE_STAGE_NONE;
};

/** Struct describing the access to a buffer. */
struct VKBufferAccess {
  VkBuffer vk_buffer;
  VkAccessFlags vk_access_flags;

  /**
   * Which pipeline stage(s) access the resource.
   *
   * When VK_PIPELINE_STAGE_NONE the node-level pipeline stage will be used as fallback.
   */
  VkPipelineStageFlags vk_pipeline_stages = VK_PIPELINE_STAGE_NONE;
};

/** Struct describing all resource accesses a draw/dispatch node has. */
struct VKResourceAccessInfo : NonCopyable {
  Vector<VKBufferAccess> buffers;
  Vector<VKImageAccess> images;

  /**
   * Extract read/write resource dependencies and add them to `node_links`.
   */
  void build_links(VKResourceStateTracker &resources, VKRenderGraphLinks &links) const;

  /**
   * Reset the instance for reuse.
   */
  void reset();
};

}  // namespace blender::gpu::render_graph
