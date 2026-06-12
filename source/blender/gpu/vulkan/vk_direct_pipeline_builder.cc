/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_direct_pipeline_builder.hh"

#include "vk_backend.hh"
#include "vk_context.hh"
#include "vk_direct_command_buffer.hh"
#include "vk_framebuffer.hh"
#include "vk_shader.hh"
#include "vk_shader_interface.hh"
#include "vk_state_manager.hh"
#include "vk_texture.hh"
#include "vk_vertex_attribute_object.hh"

namespace blender::gpu {

void VKDirectPipelineBuilder::bind_graphics_pipeline(VKDirectCommandBuffer &command_buffer,
                                                     VKContext &context,
                                                     const VKFrameBuffer &framebuffer,
                                                     GPUPrimType primitive,
                                                     VKVertexAttributeObject &vao)
{
  VKShader &shader = *unwrap(context.shader);
  VKStateManager &state_manager = context.state_manager_get();

  /* Disable non-vulkan state flags to reduce unneeded pipeline compilation. */
  state_manager.state.clip_control = 0;

  VKDevice &device = VKBackend::get().device;
  const VKExtensions &extensions = device.extensions_get();

  VKVertexInputDescriptionPool::Key vertex_input_key =
      device.vertex_input_descriptions.get_or_insert(vao.vertex_input);

  VkPipeline vk_pipeline = shader.ensure_and_get_graphics_pipeline(
      primitive,
      vertex_input_key,
      state_manager,
      framebuffer,
      context.specialization_constants_get());

  command_buffer.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);

  /* Dynamic state. */
  set_dynamic_state(command_buffer, context, framebuffer, primitive, vao);
}

void VKDirectPipelineBuilder::bind_compute_pipeline(VKDirectCommandBuffer &command_buffer,
                                                    VKContext &context)
{
  VKShader &shader = *unwrap(context.shader);
  VkPipeline vk_pipeline = shader.ensure_and_get_compute_pipeline(
      context.specialization_constants_get());

  command_buffer.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipeline);
}

void VKDirectPipelineBuilder::bind_descriptor_sets(VKDirectCommandBuffer &command_buffer,
                                                   VKContext &context,
                                                   VkPipelineBindPoint bind_point)
{
  VKShader &shader = *unwrap(context.shader);
  VKDescriptorSetTracker &descriptor_set = context.descriptor_set_get();

  if (!shader.has_descriptor_set()) {
    return;
  }

  /* Transition images to VK_IMAGE_LAYOUT_GENERAL before updating descriptors.
   * This must happen outside a render pass. Descriptors always specify GENERAL
   * for both sampled and storage images. */
  VKStateManager &state_manager = context.state_manager_get();
  const VKShaderInterface &shader_interface = shader.interface_get();
  bool needs_restart = false;
  bool did_end_rendering = false;
  for (const VKResourceBinding &resource_binding : shader_interface.resource_bindings_get()) {
    if (resource_binding.bind_type == VKBindType::IMAGE) {
      VKTexture *texture = state_manager.images_.get(resource_binding.binding);
      if (texture) {
        needs_restart = true;
        break;
      }
    }
    if (resource_binding.bind_type == VKBindType::SAMPLER) {
      const BindSpaceTextures::Elem *elem = state_manager.textures_.get(resource_binding.binding);
      if (elem && elem->resource_type == BindSpaceTextures::Type::Texture) {
        needs_restart = true;
        break;
      }
    }
  }

  if (needs_restart && command_buffer.is_rendering()) {
    command_buffer.end_rendering();
    VKFrameBuffer *framebuffer = context.active_framebuffer_get();
    if (framebuffer) {
      framebuffer->rendering_reset();
    }
    did_end_rendering = true;
  }

  if (needs_restart) {
    for (const VKResourceBinding &resource_binding : shader_interface.resource_bindings_get()) {
      if (resource_binding.bind_type == VKBindType::IMAGE) {
        VKTexture *texture = state_manager.images_.get(resource_binding.binding);
        if (texture) {
          VkImageAspectFlags aspect = to_vk_image_aspect_flag_bits(texture->device_format_get());
          command_buffer.barrier_image_to_general(texture->vk_image_handle(),
                                                  VK_ACCESS_SHADER_READ_BIT |
                                                      VK_ACCESS_SHADER_WRITE_BIT,
                                                  aspect);
        }
      }
      if (resource_binding.bind_type == VKBindType::SAMPLER) {
        const BindSpaceTextures::Elem *elem = state_manager.textures_.get(
            resource_binding.binding);
        if (elem && elem->resource_type == BindSpaceTextures::Type::Texture) {
          VKTexture *texture = static_cast<VKTexture *>(elem->resource);
          VkImageAspectFlags aspect = to_vk_image_aspect_flag_bits(texture->device_format_get());
          command_buffer.barrier_image_to_general(
              texture->vk_image_handle(), VK_ACCESS_SHADER_READ_BIT, aspect);
        }
      }
    }
  }

  if (did_end_rendering) {
    VKFrameBuffer *framebuffer = context.active_framebuffer_get();
    if (framebuffer) {
      framebuffer->rendering_ensure(context);
    }
  }

  /* In direct mode we must explicitly update and upload the descriptor set before binding,
   * unlike the render graph path where this is handled by update_pipeline_data. */
  descriptor_set.update_descriptor_set(context);

  /* The descriptor set update may have ended the render pass (e.g., via ensure_updated()
   * calling copy_buffer for staging uploads). Restart it if needed. */
  if (!command_buffer.is_rendering()) {
    VKFrameBuffer *framebuffer = context.active_framebuffer_get();
    if (framebuffer) {
      framebuffer->rendering_ensure(context);
    }
  }

  descriptor_set.upload_descriptor_sets();
  VkDescriptorSet vk_descriptor_set = descriptor_set.descriptor_sets.vk_descriptor_set;

  VkPipelineLayout layout = shader.vk_pipeline_layout;
  command_buffer.bind_descriptor_sets(bind_point, layout, 0, 1, &vk_descriptor_set, 0, nullptr);
}

void VKDirectPipelineBuilder::push_constants(VKDirectCommandBuffer &command_buffer,
                                             VKContext &context,
                                             VkShaderStageFlags stage_flags)
{
  VKShader &shader = *unwrap(context.shader);
  const VKPushConstants::Layout &layout = shader.interface_get().push_constants_layout_get();

  if (layout.storage_type_get() != VKPushConstants::StorageType::PUSH_CONSTANTS) {
    return;
  }

  VkPipelineLayout pipeline_layout = shader.vk_pipeline_layout;

  uint32_t size = layout.size_in_bytes();
  const void *data = shader.push_constants.data();

  if (size > 0) {
    command_buffer.push_constants(pipeline_layout, stage_flags, 0, size, data);
  }
}

void VKDirectPipelineBuilder::set_dynamic_state(VKDirectCommandBuffer &command_buffer,
                                                VKContext &context,
                                                const VKFrameBuffer &framebuffer,
                                                GPUPrimType primitive,
                                                VKVertexAttributeObject &vao)
{
  /* Viewports. */
  Vector<VkViewport> viewports;
  framebuffer.vk_viewports_append(viewports);
  command_buffer.set_viewport(viewports);

  /* Scissors. */
  Vector<VkRect2D> scissors;
  framebuffer.vk_render_areas_append(scissors);
  command_buffer.set_scissor(scissors);

  /* Line width. */
  set_dynamic_state_line_width(command_buffer, context, primitive);

  /* Stencil state. */
  set_dynamic_state_stencil(command_buffer, context, framebuffer);

  /* Front face (VK_EXT_extended_dynamic_state). */
  set_dynamic_state_front_face(command_buffer, context);

  /* Vertex input (VK_EXT_vertex_input_dynamic_state). */
  set_dynamic_state_vertex_input(command_buffer, vao);
}

void VKDirectPipelineBuilder::set_dynamic_state_line_width(VKDirectCommandBuffer &command_buffer,
                                                           VKContext &context,
                                                           GPUPrimType primitive)
{
  const VKStateManager &state_manager = context.state_manager_get();
  VKDevice &device = VKBackend::get().device;
  const VKExtensions &extensions = device.extensions_get();

  const bool is_line_primitive = ELEM(primitive,
                                      GPU_PRIM_LINES,
                                      GPU_PRIM_LINE_LOOP,
                                      GPU_PRIM_LINE_STRIP,
                                      GPU_PRIM_LINES_ADJ,
                                      GPU_PRIM_LINE_STRIP_ADJ);
  if (is_line_primitive) {
    float line_width = extensions.wide_lines ? state_manager.mutable_state.line_width : 1.0f;
    command_buffer.set_line_width(line_width);
  }
}

void VKDirectPipelineBuilder::set_dynamic_state_stencil(VKDirectCommandBuffer &command_buffer,
                                                        VKContext &context,
                                                        const VKFrameBuffer &framebuffer)
{
  const VKStateManager &state_manager = context.state_manager_get();

  if (framebuffer.stencil_attachment_format_get() != VK_FORMAT_UNDEFINED &&
      state_manager.state.stencil_test != GPU_STENCIL_NONE)
  {
    command_buffer.set_stencil_compare_mask(state_manager.mutable_state.stencil_compare_mask);
    command_buffer.set_stencil_reference(state_manager.mutable_state.stencil_reference);
    command_buffer.set_stencil_write_mask(state_manager.mutable_state.stencil_write_mask);
  }
}

void VKDirectPipelineBuilder::set_dynamic_state_front_face(VKDirectCommandBuffer &command_buffer,
                                                           VKContext &context)
{
  VKDevice &device = VKBackend::get().device;
  const VKExtensions &extensions = device.extensions_get();

  if (extensions.extended_dynamic_state) {
    const VKStateManager &state_manager = context.state_manager_get();
    VkFrontFace front_face = state_manager.state.invert_facing ? VK_FRONT_FACE_COUNTER_CLOCKWISE :
                                                                 VK_FRONT_FACE_CLOCKWISE;
    command_buffer.set_front_face(front_face);
  }
}

void VKDirectPipelineBuilder::set_dynamic_state_vertex_input(VKDirectCommandBuffer &command_buffer,
                                                             VKVertexAttributeObject &vao)
{
  VKDevice &device = VKBackend::get().device;
  const VKExtensions &extensions = device.extensions_get();

  if (extensions.vertex_input_dynamic_state) {
    VKVertexInputDescriptionPool::Key vertex_input_key =
        device.vertex_input_descriptions.get_or_insert(vao.vertex_input);
    const VKVertexInputDescription &desc = device.vertex_input_descriptions.get(vertex_input_key);
    command_buffer.set_vertex_input(desc.bindings, desc.attributes);
  }
}

}  // namespace blender::gpu
