/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_resource_pool.hh"
#include "vk_backend.hh"

namespace blender::gpu {

void VKResourcePool::init(VKDevice &device)
{
  descriptor_pools.init(device);

  VkCommandPoolCreateInfo vk_command_pool_create_info_ = {};
  vk_command_pool_create_info_.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  vk_command_pool_create_info_.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                       VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  vk_command_pool_create_info_.queueFamilyIndex = 0;
  vk_command_pool_create_info_.queueFamilyIndex = device.queue_family_get();
  vkCreateCommandPool(
      device.vk_handle(), &vk_command_pool_create_info_, nullptr, &vk_command_pool_);
}

void VKResourcePool::deinit(VKDevice &device)
{
  immediate.deinit(device);
  discard_pool.deinit(device);

  auto free_command_buffers = [&](Vector<VkCommandBuffer> &vk_command_buffers) {
    vkFreeCommandBuffers(device.vk_handle(),
                         vk_command_pool_,
                         vk_command_buffers.size(),
                         vk_command_buffers.data());
    vk_command_buffers.clear();
  };
  free_command_buffers(primary_command_buffers_);
  free_command_buffers(primary_command_buffers_discarded_);
  free_command_buffers(secondary_command_buffers_);
  free_command_buffers(secondary_command_buffers_discarded_);

  vkDestroyCommandPool(device.vk_handle(), vk_command_pool_, nullptr);
  vk_command_pool_ = VK_NULL_HANDLE;
}

void VKResourcePool::reset(VKDevice & /*device*/)
{
  descriptor_pools.reset();
  immediate.reset();

  auto recycle_command_buffers = [](Vector<VkCommandBuffer> &command_buffers,
                                    Vector<VkCommandBuffer> &command_buffers_discarded) {
    while (!command_buffers_discarded.is_empty()) {
      VkCommandBuffer vk_command_buffer = command_buffers_discarded.pop_last();
      vkResetCommandBuffer(vk_command_buffer, 0);
      command_buffers.append(vk_command_buffer);
    }
  };
  recycle_command_buffers(primary_command_buffers_, primary_command_buffers_discarded_);
  recycle_command_buffers(secondary_command_buffers_, secondary_command_buffers_discarded_);
}

VkCommandBuffer VKResourcePool::allocate_command_buffer(
    VKDevice &device, VkCommandBufferLevel vk_command_buffer_level)
{
  Vector<VkCommandBuffer> &command_buffers = vk_command_buffer_level ==
                                                     VK_COMMAND_BUFFER_LEVEL_PRIMARY ?
                                                 primary_command_buffers_ :
                                                 secondary_command_buffers_;
  Vector<VkCommandBuffer> &discard_pool = vk_command_buffer_level ==
                                                  VK_COMMAND_BUFFER_LEVEL_PRIMARY ?
                                              primary_command_buffers_discarded_ :
                                              secondary_command_buffers_discarded_;

  if (command_buffers.is_empty()) {
    command_buffers.append_n_times(VK_NULL_HANDLE, 256);
    VkCommandBufferAllocateInfo command_buffer_allocation_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr,
        vk_command_pool_,
        vk_command_buffer_level,
        256};
    vkAllocateCommandBuffers(
        device.vk_handle(), &command_buffer_allocation_info, command_buffers.data());
  }

  VkCommandBuffer vk_command_buffer = command_buffers.pop_last();
  discard_pool.append(vk_command_buffer);
  return vk_command_buffer;
}

void VKDiscardPool::deinit(VKDevice &device)
{
  destroy_discarded_resources(device);
}

void VKDiscardPool::move_data(VKDiscardPool &src_pool)
{
  std::scoped_lock mutex(mutex_);
  std::scoped_lock mutex_src(src_pool.mutex_);
  buffers_.extend(std::move(src_pool.buffers_));
  image_views_.extend(std::move(src_pool.image_views_));
  images_.extend(std::move(src_pool.images_));
  shader_modules_.extend(std::move(src_pool.shader_modules_));
  pipeline_layouts_.extend(std::move(src_pool.pipeline_layouts_));
  framebuffers_.extend(std::move(src_pool.framebuffers_));
  render_passes_.extend(std::move(src_pool.render_passes_));
}

void VKDiscardPool::discard_image(VkImage vk_image, VmaAllocation vma_allocation)
{
  std::scoped_lock mutex(mutex_);
  images_.append(std::pair(vk_image, vma_allocation));
}

void VKDiscardPool::discard_image_view(VkImageView vk_image_view)
{
  std::scoped_lock mutex(mutex_);
  image_views_.append(vk_image_view);
}

void VKDiscardPool::discard_buffer(VkBuffer vk_buffer, VmaAllocation vma_allocation)
{
  std::scoped_lock mutex(mutex_);
  buffers_.append(std::pair(vk_buffer, vma_allocation));
}

void VKDiscardPool::discard_shader_module(VkShaderModule vk_shader_module)
{
  std::scoped_lock mutex(mutex_);
  shader_modules_.append(vk_shader_module);
}
void VKDiscardPool::discard_pipeline_layout(VkPipelineLayout vk_pipeline_layout)
{
  std::scoped_lock mutex(mutex_);
  pipeline_layouts_.append(vk_pipeline_layout);
}

void VKDiscardPool::discard_framebuffer(VkFramebuffer vk_framebuffer)
{
  std::scoped_lock mutex(mutex_);
  framebuffers_.append(vk_framebuffer);
}

void VKDiscardPool::discard_render_pass(VkRenderPass vk_render_pass)
{
  std::scoped_lock mutex(mutex_);
  render_passes_.append(vk_render_pass);
}

void VKDiscardPool::destroy_discarded_resources(VKDevice &device)
{
  std::scoped_lock mutex(mutex_);

  while (!image_views_.is_empty()) {
    VkImageView vk_image_view = image_views_.pop_last();
    vkDestroyImageView(device.vk_handle(), vk_image_view, nullptr);
  }

  while (!images_.is_empty()) {
    std::pair<VkImage, VmaAllocation> image_allocation = images_.pop_last();
    device.resources.remove_image(image_allocation.first);
    vmaDestroyImage(device.mem_allocator_get(), image_allocation.first, image_allocation.second);
  }

  while (!buffers_.is_empty()) {
    std::pair<VkBuffer, VmaAllocation> buffer_allocation = buffers_.pop_last();
    device.resources.remove_buffer(buffer_allocation.first);
    vmaDestroyBuffer(
        device.mem_allocator_get(), buffer_allocation.first, buffer_allocation.second);
  }

  while (!pipeline_layouts_.is_empty()) {
    VkPipelineLayout vk_pipeline_layout = pipeline_layouts_.pop_last();
    vkDestroyPipelineLayout(device.vk_handle(), vk_pipeline_layout, nullptr);
  }

  while (!shader_modules_.is_empty()) {
    VkShaderModule vk_shader_module = shader_modules_.pop_last();
    vkDestroyShaderModule(device.vk_handle(), vk_shader_module, nullptr);
  }

  while (!framebuffers_.is_empty()) {
    VkFramebuffer vk_framebuffer = framebuffers_.pop_last();
    vkDestroyFramebuffer(device.vk_handle(), vk_framebuffer, nullptr);
  }

  while (!render_passes_.is_empty()) {
    VkRenderPass vk_render_pass = render_passes_.pop_last();
    vkDestroyRenderPass(device.vk_handle(), vk_render_pass, nullptr);
  }
}

}  // namespace blender::gpu
