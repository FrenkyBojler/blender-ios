/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_uniform_buffer.hh"
#include "vk_context.hh"
#include "vk_shader.hh"
#include "vk_shader_interface.hh"
#include "vk_staging_buffer.hh"
#include "vk_state_manager.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"gpu.vulkan"};

namespace gpu {

void VKUniformBuffer::update(const void *data)
{
#if 1
  const bool reallocate = buffer_.is_allocated() && buffer_.is_mapped() && data && data_uploaded_;
  if (reallocate) {
    /* Data could still be use */
    buffer_.free();
    data_uploaded_ = false;
  }
#endif

  const bool new_allocation = !buffer_.is_allocated();
  if (new_allocation) {
    allocate();
  }

  if (data) {
    /* Immediate updates can only be used when the buffer is newly allocated. The reason is that
     * the buffer can still be written to by a clear command inside the render graph. */
    if (new_allocation && buffer_.is_mapped()) {
      buffer_.update_immediately(data);
    }
    else {
      void *data_copy = MEM_new_uninitialized(size_in_bytes_, __func__);
      memcpy(data_copy, data, size_in_bytes_);
      VKContext &context = *VKContext::get();
      buffer_.update_render_graph(context, data_copy);
    }
    data_uploaded_ = true;
  }
}

void VKUniformBuffer::allocate()
{
  buffer_.create(size_in_bytes_,
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT,
                 0.8f);
  debug::object_label(buffer_.vk_handle(), name_);
}

void VKUniformBuffer::clear_to_zero()
{
  if (!buffer_.is_allocated()) {
    allocate();
  }
  VKContext &context = *VKContext::get();
  buffer_.clear(context, 0);
  data_uploaded_ = true;
}

void VKUniformBuffer::ensure_updated()
{
  /* Upload attached data, during bind time. */
  if (data_) {
    update(data_);
    MEM_delete_void(data_);
    data_ = nullptr;
  }
  else {
    if (!buffer_.is_allocated()) {
      allocate();
    }
  }
}

void VKUniformBuffer::bind(int slot)
{
  VKContext &context = *VKContext::get();
  context.state_manager_get().uniform_buffer_bind(this, slot);
}

void VKUniformBuffer::bind_as_ssbo(int slot)
{
  VKContext &context = *VKContext::get();
  context.state_manager_get().storage_buffer_bind(
      BindSpaceStorageBuffers::Type::UniformBuffer, this, slot);
}

void VKUniformBuffer::unbind()
{
  const VKContext *context = VKContext::get();
  if (context != nullptr) {
    VKStateManager &state_manager = context->state_manager_get();
    state_manager.uniform_buffer_unbind(this);
    state_manager.storage_buffer_unbind(this);
  }
}

}  // namespace gpu
}  // namespace blender
