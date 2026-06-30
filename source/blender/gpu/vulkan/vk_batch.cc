/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_batch.hh"

#include "vk_backend.hh"
#include "vk_batch.hh"

#include "render_graph/nodes/vk_pipeline_data.hh"
#include "vk_context.hh"
#include "vk_framebuffer.hh"
#include "vk_index_buffer.hh"
#include "vk_state_manager.hh"
#include "vk_storage_buffer.hh"
#include "vk_vertex_attribute_object.hh"
#include "vk_vertex_buffer.hh"

namespace blender::gpu {

void VKBatch::upload_data()
{
  VKIndexBuffer *index_buffer = index_buffer_get();
  if (index_buffer) {
    index_buffer->upload_data();
  }
  for (int v = 0; v < GPU_BATCH_VBO_MAX_LEN; v++) {
    VKVertexBuffer *vbo = vertex_buffer_get(v);
    if (vbo) {
      vbo->upload();
    }
  }
}

void VKBatch::draw(int vertex_first, int vertex_count, int instance_first, int instance_count)
{
  VKContext &context = *VKContext::get();
  render_graph::VKResourceAccessInfo &resource_access_info = context.reset_and_get_access_info();

  VKIndexBuffer *index_buffer = index_buffer_get();
  const bool draw_indexed = index_buffer != nullptr;

  upload_data();
  VKFrameBuffer &framebuffer = *context.active_framebuffer_get();
  framebuffer.rendering_ensure(context);

  if (draw_indexed) {
    render_graph::VKDrawIndexedNode::CreateInfo draw_indexed(resource_access_info);
    draw_indexed.node_data.index_count = vertex_count;
    draw_indexed.node_data.instance_count = instance_count;
    draw_indexed.node_data.first_index = index_buffer->index_start_get() + vertex_first;
    draw_indexed.node_data.vertex_offset = index_buffer->index_base_get();
    draw_indexed.node_data.first_instance = instance_first;

    draw_indexed.node_data.index_buffer.buffer = index_buffer->vk_handle();
    draw_indexed.node_data.index_buffer.index_type = index_buffer->vk_index_type();

    VKVertexAttributeBatchCache &cache = context.vertex_attribute_batch_cache_;
    VKVertexInputDescriptionPool::Key vertex_input_key = cache.try_get(this->shader, verts);

    if (vertex_input_key != VKVertexInputDescriptionPool::invalid_key) {
      /* Cache hit: use cached vertex buffers and key directly. */
      draw_indexed.node_data.vertex_buffers = *cache.vertex_buffers_get();
      if (VKBackend::get().device.extensions_get().vertex_input_dynamic_state) {
        draw_indexed.node_data.graphics.vertex_input_description = vertex_input_key;
      }
    }
    else {
      /* Cache miss: build vertex attribute object. */
      VKVertexAttributeObject vao;
      vao.update_bindings(context, *this);
      vao.bind(draw_indexed.node_data.vertex_buffers);
      vertex_input_key = VKBackend::get().device.vertex_input_descriptions.get_or_insert(
          vao.vertex_input);
      cache.update(this->shader, verts, vertex_input_key, draw_indexed.node_data.vertex_buffers);

      if (VKBackend::get().device.extensions_get().vertex_input_dynamic_state) {
        draw_indexed.node_data.graphics.vertex_input_description = vertex_input_key;
      }
    }

    context.update_pipeline_data(
        framebuffer, prim_type, vertex_input_key, draw_indexed.node_data.graphics);

    context.render_graph().add_node(draw_indexed);
  }
  else {
    render_graph::VKDrawNode::CreateInfo draw(resource_access_info);
    draw.node_data.vertex_count = vertex_count;
    draw.node_data.instance_count = instance_count;
    draw.node_data.first_vertex = vertex_first;
    draw.node_data.first_instance = instance_first;

    VKVertexAttributeBatchCache &cache = context.vertex_attribute_batch_cache_;
    VKVertexInputDescriptionPool::Key vertex_input_key = cache.try_get(this->shader, verts);

    if (vertex_input_key != VKVertexInputDescriptionPool::invalid_key) {
      /* Cache hit: use cached vertex buffers and key directly. */
      draw.node_data.vertex_buffers = *cache.vertex_buffers_get();
      if (VKBackend::get().device.extensions_get().vertex_input_dynamic_state) {
        draw.node_data.graphics.vertex_input_description = vertex_input_key;
      }
    }
    else {
      /* Cache miss: build vertex attribute object. */
      VKVertexAttributeObject vao;
      vao.update_bindings(context, *this);
      vao.bind(draw.node_data.vertex_buffers);
      vertex_input_key = VKBackend::get().device.vertex_input_descriptions.get_or_insert(
          vao.vertex_input);
      cache.update(this->shader, verts, vertex_input_key, draw.node_data.vertex_buffers);

      if (VKBackend::get().device.extensions_get().vertex_input_dynamic_state) {
        draw.node_data.graphics.vertex_input_description = vertex_input_key;
      }
    }

    context.update_pipeline_data(
        framebuffer, prim_type, vertex_input_key, draw.node_data.graphics);

    context.render_graph().add_node(draw);
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
  render_graph::VKResourceAccessInfo &resource_access_info = context.reset_and_get_access_info();

  upload_data();
  VKFrameBuffer &framebuffer = *context.active_framebuffer_get();
  framebuffer.rendering_ensure(context);

  VKVertexAttributeBatchCache &cache = context.vertex_attribute_batch_cache_;

  VKIndexBuffer *index_buffer = index_buffer_get();
  if (index_buffer) {
    render_graph::VKDrawIndexedIndirectNode::CreateInfo draw_indexed_indirect(
        resource_access_info);
    draw_indexed_indirect.node_data.indirect_buffer = indirect_buffer;
    draw_indexed_indirect.node_data.offset = offset;
    draw_indexed_indirect.node_data.draw_count = count;
    draw_indexed_indirect.node_data.stride = stride;

    draw_indexed_indirect.node_data.index_buffer.buffer = index_buffer->vk_handle();
    draw_indexed_indirect.node_data.index_buffer.index_type = index_buffer->vk_index_type();

    VKVertexInputDescriptionPool::Key vertex_input_key = cache.try_get(this->shader, verts);

    if (vertex_input_key != VKVertexInputDescriptionPool::invalid_key) {
      draw_indexed_indirect.node_data.vertex_buffers = *cache.vertex_buffers_get();
      if (VKBackend::get().device.extensions_get().vertex_input_dynamic_state) {
        draw_indexed_indirect.node_data.graphics.vertex_input_description = vertex_input_key;
      }
    }
    else {
      VKVertexAttributeObject vao;
      vao.update_bindings(context, *this);
      vao.bind(draw_indexed_indirect.node_data.vertex_buffers);
      vertex_input_key = VKBackend::get().device.vertex_input_descriptions.get_or_insert(
          vao.vertex_input);
      cache.update(
          this->shader, verts, vertex_input_key, draw_indexed_indirect.node_data.vertex_buffers);

      if (VKBackend::get().device.extensions_get().vertex_input_dynamic_state) {
        draw_indexed_indirect.node_data.graphics.vertex_input_description = vertex_input_key;
      }
    }

    context.update_pipeline_data(
        framebuffer, prim_type, vertex_input_key, draw_indexed_indirect.node_data.graphics);

    context.render_graph().add_node(draw_indexed_indirect);
  }
  else {
    render_graph::VKDrawIndirectNode::CreateInfo draw(resource_access_info);
    draw.node_data.indirect_buffer = indirect_buffer;
    draw.node_data.offset = offset;
    draw.node_data.draw_count = count;
    draw.node_data.stride = stride;

    VKVertexInputDescriptionPool::Key vertex_input_key = cache.try_get(this->shader, verts);

    if (vertex_input_key != VKVertexInputDescriptionPool::invalid_key) {
      draw.node_data.vertex_buffers = *cache.vertex_buffers_get();
      if (VKBackend::get().device.extensions_get().vertex_input_dynamic_state) {
        draw.node_data.graphics.vertex_input_description = vertex_input_key;
      }
    }
    else {
      VKVertexAttributeObject vao;
      vao.update_bindings(context, *this);
      vao.bind(draw.node_data.vertex_buffers);
      vertex_input_key = VKBackend::get().device.vertex_input_descriptions.get_or_insert(
          vao.vertex_input);
      cache.update(this->shader, verts, vertex_input_key, draw.node_data.vertex_buffers);

      if (VKBackend::get().device.extensions_get().vertex_input_dynamic_state) {
        draw.node_data.graphics.vertex_input_description = vertex_input_key;
      }
    }

    context.update_pipeline_data(
        framebuffer, prim_type, vertex_input_key, draw.node_data.graphics);

    context.render_graph().add_node(draw);
  }
}

}  // namespace blender::gpu
