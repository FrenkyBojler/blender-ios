/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_batch.hh"

#ifdef WITH_VULKAN_BACKEND_RENDER_GRAPH
#  include "render_graph/nodes/vk_pipeline_data.hh"
#endif
#include "vk_context.hh"
#include "vk_direct_pipeline_builder.hh"
#include "vk_framebuffer.hh"
#include "vk_index_buffer.hh"
#include "vk_state_manager.hh"
#include "vk_storage_buffer.hh"
#include "vk_vertex_attribute_object.hh"
#include "vk_vertex_buffer.hh"

namespace blender::gpu {

void VKBatch::draw(int vertex_first, int vertex_count, int instance_first, int instance_count)
{
  VKContext &context = *VKContext::get();
#ifdef WITH_VULKAN_BACKEND_RENDER_GRAPH
  render_graph::VKResourceAccessInfo &resource_access_info = context.reset_and_get_access_info();
#endif
  VKVertexAttributeObject vao;
  vao.update_bindings(context, *this);

  VKIndexBuffer *index_buffer = index_buffer_get();
  const bool draw_indexed = index_buffer != nullptr;

  /* Upload geometry */
  if (draw_indexed) {
    index_buffer->upload_data();
  }
  VKFrameBuffer &framebuffer = *context.active_framebuffer_get();
  framebuffer.rendering_ensure(context);

  if (draw_indexed) {
#ifdef WITH_VULKAN_BACKEND_RENDER_GRAPH
    render_graph::VKDrawIndexedNode::CreateInfo draw_indexed(resource_access_info);
    draw_indexed.node_data.index_count = vertex_count;
    draw_indexed.node_data.instance_count = instance_count;
    draw_indexed.node_data.first_index = index_buffer->index_start_get() + vertex_first;
    draw_indexed.node_data.vertex_offset = index_buffer->index_base_get();
    draw_indexed.node_data.first_instance = instance_first;

    draw_indexed.node_data.index_buffer.buffer = index_buffer->vk_handle();
    draw_indexed.node_data.index_buffer.index_type = index_buffer->vk_index_type();
    vao.bind(draw_indexed.node_data.vertex_buffers);
    context.update_pipeline_data(framebuffer, prim_type, vao, draw_indexed.node_data.graphics);

    context.render_graph().add_node(draw_indexed);
#else
    VKDirectPipelineBuilder::bind_graphics_pipeline(
        context.command_buffer(), context, framebuffer, prim_type, vao);
    vao.bind(context.command_buffer());
    VKDirectPipelineBuilder::bind_descriptor_sets(
        context.command_buffer(), context, VK_PIPELINE_BIND_POINT_GRAPHICS);
    VKDirectPipelineBuilder::push_constants(context.command_buffer(), context);
    context.command_buffer().bind_index_buffer(
        index_buffer->vk_handle(), 0, index_buffer->vk_index_type());
    context.command_buffer().draw_indexed(vertex_count,
                                          instance_count,
                                          index_buffer->index_start_get() + vertex_first,
                                          index_buffer->index_base_get(),
                                          instance_first);
#endif
  }
  else {
#ifdef WITH_VULKAN_BACKEND_RENDER_GRAPH
    render_graph::VKDrawNode::CreateInfo draw(resource_access_info);
    draw.node_data.vertex_count = vertex_count;
    draw.node_data.instance_count = instance_count;
    draw.node_data.first_vertex = vertex_first;
    draw.node_data.first_instance = instance_first;

    vao.bind(draw.node_data.vertex_buffers);
    context.update_pipeline_data(framebuffer, prim_type, vao, draw.node_data.graphics);

    context.render_graph().add_node(draw);
#else
    VKDirectPipelineBuilder::bind_graphics_pipeline(
        context.command_buffer(), context, framebuffer, prim_type, vao);
    vao.bind(context.command_buffer());
    VKDirectPipelineBuilder::bind_descriptor_sets(
        context.command_buffer(), context, VK_PIPELINE_BIND_POINT_GRAPHICS);
    VKDirectPipelineBuilder::push_constants(context.command_buffer(), context);
    context.command_buffer().draw(vertex_count, instance_count, vertex_first, instance_first);
#endif
  }
}

void VKBatch::draw_indirect(StorageBuf *indirect_buf, intptr_t offset)
{
  multi_draw_indirect(indirect_buf, 1, offset, 0);
}

void VKBatch::multi_draw_indirect(StorageBuf *indirect_buf,
                                  const int count,
                                  const intptr_t offset,
                                  const intptr_t stride)
{
  VKStorageBuffer &indirect_buffer = *unwrap(unwrap(indirect_buf));
  multi_draw_indirect(indirect_buffer.vk_handle(), count, offset, stride);
}

void VKBatch::multi_draw_indirect(const VkBuffer indirect_buffer,
                                  const int count,
                                  const intptr_t offset,
                                  const intptr_t stride)
{
  VKContext &context = *VKContext::get();
#ifdef WITH_VULKAN_BACKEND_RENDER_GRAPH
  render_graph::VKResourceAccessInfo &resource_access_info = context.reset_and_get_access_info();
#endif
  VKVertexAttributeObject vao;
  vao.update_bindings(context, *this);

  VKIndexBuffer *index_buffer = index_buffer_get();
  const bool draw_indexed = index_buffer != nullptr;

  /* Upload geometry */
  if (draw_indexed) {
    index_buffer->upload_data();
  }
  VKFrameBuffer &framebuffer = *context.active_framebuffer_get();
  framebuffer.rendering_ensure(context);

  if (draw_indexed) {
#ifdef WITH_VULKAN_BACKEND_RENDER_GRAPH
    render_graph::VKDrawIndexedIndirectNode::CreateInfo draw_indexed_indirect(
        resource_access_info);
    draw_indexed_indirect.node_data.indirect_buffer = indirect_buffer;
    draw_indexed_indirect.node_data.offset = offset;
    draw_indexed_indirect.node_data.draw_count = count;
    draw_indexed_indirect.node_data.stride = stride;

    draw_indexed_indirect.node_data.index_buffer.buffer = index_buffer->vk_handle();
    draw_indexed_indirect.node_data.index_buffer.index_type = index_buffer->vk_index_type();
    vao.bind(draw_indexed_indirect.node_data.vertex_buffers);
    context.update_pipeline_data(
        framebuffer, prim_type, vao, draw_indexed_indirect.node_data.graphics);

    context.render_graph().add_node(draw_indexed_indirect);
#else
    VKDirectPipelineBuilder::bind_graphics_pipeline(
        context.command_buffer(), context, framebuffer, prim_type, vao);
    vao.bind(context.command_buffer());
    VKDirectPipelineBuilder::bind_descriptor_sets(
        context.command_buffer(), context, VK_PIPELINE_BIND_POINT_GRAPHICS);
    VKDirectPipelineBuilder::push_constants(context.command_buffer(), context);
    context.command_buffer().bind_index_buffer(
        index_buffer->vk_handle(), 0, index_buffer->vk_index_type());
    context.command_buffer().draw_indexed_indirect(indirect_buffer, offset, count, stride);
#endif
  }
  else {
#ifdef WITH_VULKAN_BACKEND_RENDER_GRAPH
    render_graph::VKDrawIndirectNode::CreateInfo draw(resource_access_info);
    draw.node_data.indirect_buffer = indirect_buffer;
    draw.node_data.offset = offset;
    draw.node_data.draw_count = count;
    draw.node_data.stride = stride;

    vao.bind(draw.node_data.vertex_buffers);
    context.update_pipeline_data(framebuffer, prim_type, vao, draw.node_data.graphics);

    context.render_graph().add_node(draw);
#else
    VKDirectPipelineBuilder::bind_graphics_pipeline(
        context.command_buffer(), context, framebuffer, prim_type, vao);
    vao.bind(context.command_buffer());
    VKDirectPipelineBuilder::bind_descriptor_sets(
        context.command_buffer(), context, VK_PIPELINE_BIND_POINT_GRAPHICS);
    VKDirectPipelineBuilder::push_constants(context.command_buffer(), context);
    context.command_buffer().draw_indirect(indirect_buffer, offset, count, stride);
#endif
  }
}

}  // namespace blender::gpu
