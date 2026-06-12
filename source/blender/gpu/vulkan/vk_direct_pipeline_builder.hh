/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "vk_common.hh"

namespace blender::gpu {

class VKContext;
class VKShader;
class VKFrameBuffer;
class VKVertexAttributeObject;
class VKStateManager;
class VKDescriptorSetTracker;
class VKDirectCommandBuffer;

/**
 * Helper for binding graphics/compute pipeline state directly to a command buffer.
 *
 * Used in direct mode (WITHOUT_VULKAN_BACKEND_RENDER_GRAPH) to replicate the
 * pipeline binding logic that lives in render graph node types.
 */
struct VKDirectPipelineBuilder {

  /**
   * Bind a graphics pipeline and all associated dynamic state.
   */
  static void bind_graphics_pipeline(VKDirectCommandBuffer &command_buffer,
                                     VKContext &context,
                                     const VKFrameBuffer &framebuffer,
                                     GPUPrimType primitive,
                                     VKVertexAttributeObject &vao);

  /**
   * Bind a compute pipeline.
   */
  static void bind_compute_pipeline(VKDirectCommandBuffer &command_buffer, VKContext &context);

  /**
   * Bind descriptor sets for the current shader.
   */
  static void bind_descriptor_sets(VKDirectCommandBuffer &command_buffer,
                                   VKContext &context,
                                   VkPipelineBindPoint bind_point);

  /**
   * Push constants for the current shader.
   */
  static void push_constants(VKDirectCommandBuffer &command_buffer,
                             VKContext &context,
                             VkShaderStageFlags stage_flags);

  /**
   * Apply dynamic state (viewports, scissors, line width, front face, stencil, vertex input).
   */
  static void set_dynamic_state(VKDirectCommandBuffer &command_buffer,
                                VKContext &context,
                                const VKFrameBuffer &framebuffer,
                                GPUPrimType primitive,
                                VKVertexAttributeObject &vao);

 private:
  static void set_dynamic_state_line_width(VKDirectCommandBuffer &command_buffer,
                                           VKContext &context,
                                           GPUPrimType primitive);
  static void set_dynamic_state_stencil(VKDirectCommandBuffer &command_buffer,
                                        VKContext &context,
                                        const VKFrameBuffer &framebuffer);
  static void set_dynamic_state_front_face(VKDirectCommandBuffer &command_buffer,
                                           VKContext &context);
  static void set_dynamic_state_vertex_input(VKDirectCommandBuffer &command_buffer,
                                             VKVertexAttributeObject &vao);
};

}  // namespace blender::gpu
