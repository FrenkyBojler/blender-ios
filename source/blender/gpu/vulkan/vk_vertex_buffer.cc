/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "MEM_guardedalloc.h"

#include "vk_data_conversion.hh"
#include "vk_shader.hh"
#include "vk_shader_interface.hh"
#include "vk_staging_buffer.hh"
#include "vk_state_manager.hh"
#include "vk_vertex_buffer.hh"

namespace blender::gpu {

VKVertexBuffer::~VKVertexBuffer()
{
  release_data();
}

void VKVertexBuffer::bind_as_ssbo(uint binding)
{
  VKContext &context = *VKContext::get();
  VKStateManager &state_manager = context.state_manager_get();
  state_manager.storage_buffer_bind(BindSpaceStorageBuffers::Type::VertexBuffer, this, binding);
}

void VKVertexBuffer::bind_as_texture(uint binding)
{
  VKContext &context = *VKContext::get();
  VKStateManager &state_manager = context.state_manager_get();
  state_manager.texel_buffer_bind(*this, binding);
}

void VKVertexBuffer::ensure_updated()
{
  upload_data();
}

void VKVertexBuffer::ensure_buffer_view()
{
  if (vk_buffer_view_ != VK_NULL_HANDLE) {
    return;
  }

  VkBufferViewCreateInfo buffer_view_info = {};
  eGPUTextureFormat texture_format = to_texture_format(&format);

  buffer_view_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  buffer_view_info.buffer = buffer_.vk_handle();
  buffer_view_info.format = to_vk_format(texture_format);
  buffer_view_info.range = buffer_.size_in_bytes();

  const VKDevice &device = VKBackend::get().device;
  vkCreateBufferView(device.vk_handle(), &buffer_view_info, nullptr, &vk_buffer_view_);
  debug::object_label(vk_buffer_view_, "VertexBufferView");
}

void VKVertexBuffer::wrap_handle(uint64_t /*handle*/)
{
  NOT_YET_IMPLEMENTED
}

void VKVertexBuffer::update_sub(uint start_offset, uint data_size_in_bytes, const void *data)
{
  device_format_ensure();
  if (buffer_.is_mapped() && !vertex_format_converter.needs_conversion()) {
    buffer_.update_sub_immediately(start_offset, data_size_in_bytes, data);
  }
  else {
    VKContext &context = *VKContext::get();
    VKStagingBuffer staging_buffer(
        buffer_, VKStagingBuffer::Direction::HostToDevice, start_offset, data_size_in_bytes);
    if (vertex_format_converter.needs_conversion()) {
      vertex_format_converter.convert(staging_buffer.host_buffer_get().mapped_memory_get(),
                                      data_,
                                      data_size_in_bytes / vertex_len);
    }
    else {
      memcpy(staging_buffer.host_buffer_get().mapped_memory_get(), data, data_size_in_bytes);
    }
    staging_buffer.copy_to_device(context);
  }
}

void VKVertexBuffer::read(void *data) const
{
  VKContext &context = *VKContext::get();
  if (buffer_.is_mapped()) {
    buffer_.read(context, data);
    return;
  }

  VKStagingBuffer staging_buffer(buffer_, VKStagingBuffer::Direction::DeviceToHost);
  staging_buffer.copy_from_device(context);
  staging_buffer.host_buffer_get().read(context, data);
}

void VKVertexBuffer::acquire_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }

  BLI_assert(data_ == nullptr);
  VKDevice &device = VKBackend::get().device;
  VkDeviceSize alignment =
      device.physical_device_external_memory_host_properties_get().minImportedHostPointerAlignment;
  data_ = (uchar *)MEM_mallocN_aligned(
      ceil_to_multiple_ul(size_alloc_get(), alignment), alignment, __func__);
}

void VKVertexBuffer::resize_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }
  BLI_assert(false);
  data_ = (uchar *)MEM_reallocN(data_, sizeof(uchar) * this->size_alloc_get());
}

void VKVertexBuffer::release_data()
{
  if (vk_buffer_view_ != VK_NULL_HANDLE) {
    VKDiscardPool::discard_pool_get().discard_buffer_view(vk_buffer_view_);
    vk_buffer_view_ = VK_NULL_HANDLE;
  }

  MEM_SAFE_FREE(data_);
}

void VKVertexBuffer::upload_data_direct(const VKBuffer &host_buffer)
{
  device_format_ensure();
  if (vertex_format_converter.needs_conversion()) {
    if (G.debug & G_DEBUG_GPU) {
      std::cout << "PERFORMANCE: Vertex buffer requires conversion.\n";
    }
    vertex_format_converter.convert(host_buffer.mapped_memory_get(), data_, vertex_len);
    host_buffer.flush();
  }
  else {
    host_buffer.update_immediately(data_);
  }
}

void VKVertexBuffer::upload_data_via_staging_buffer(VKContext &context)
{
  VKStagingBuffer staging_buffer(buffer_, VKStagingBuffer::Direction::HostToDevice);
  upload_data_direct(staging_buffer.host_buffer_get());
  staging_buffer.copy_to_device(context);
}

void VKVertexBuffer::upload_data()
{
  device_format_ensure();
  const bool allocated = buffer_.is_allocated();
  const bool upload_required = (usage_ != GPU_USAGE_DEVICE_ONLY) &&
                               bool(flag & GPU_VERTBUF_DATA_DIRTY);
  const bool conversion_needed = vertex_format_converter.needs_conversion();
  BLI_assert(!conversion_needed);

  VKDevice &device = VKBackend::get().device;

  VkBufferUsageFlags vk_buffer_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  if (!allocated && upload_required) {
    VkDeviceSize alignment = device.physical_device_external_memory_host_properties_get()
                                 .minImportedHostPointerAlignment;

    buffer_.import_host_pointer(
        ceil_to_multiple_ul(size_alloc_get(), alignment), vk_buffer_usage, data_);
    debug::object_label(buffer_.vk_handle(), "VertexBufferHostMemory");

    flag &= ~GPU_VERTBUF_DATA_DIRTY;
    flag |= GPU_VERTBUF_DATA_UPLOADED;

    if (usage_ == GPU_USAGE_STATIC) {
      /* data_ is used as backed memory of the VkBuffer. */
      data_ = nullptr;
    }
    return;
  }
  else if (!allocated && !upload_required) {
    /* Device only allocation. */
    buffer_.create(size_alloc_get(),
                   vk_buffer_usage,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                   0,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    debug::object_label(buffer_.vk_handle(), "VertexBufferDeviceOnly");
  }
  else if (allocated && upload_required) {
    // Nothing to do data_ the same as the backed memory of the buffer.
    flag &= ~GPU_VERTBUF_DATA_DIRTY;
    flag |= GPU_VERTBUF_DATA_UPLOADED;
  }
}

void VKVertexBuffer::duplicate_data(VertBuf * /*dst*/)
{
  NOT_YET_IMPLEMENTED
}

void VKVertexBuffer::device_format_ensure()
{
  if (!vertex_format_converter.is_initialized()) {
    const VKWorkarounds &workarounds = VKBackend::get().device.workarounds_get();
    vertex_format_converter.init(&format, workarounds);
  }
}

const GPUVertFormat &VKVertexBuffer::device_format_get() const
{
  return vertex_format_converter.device_format_get();
}

void VKVertexBuffer::allocate()
{
  VkBufferUsageFlags vk_buffer_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_.create(size_alloc_get(),
                 vk_buffer_usage,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  debug::object_label(buffer_.vk_handle(), "VertexBuffer");
}

}  // namespace blender::gpu
