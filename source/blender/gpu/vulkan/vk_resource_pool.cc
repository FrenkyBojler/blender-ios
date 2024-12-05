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
}

void VKResourcePool::deinit(VKDevice &device)
{
  immediate.deinit(device);
  discard_pool.deinit(device);
}

void VKResourcePool::reset()
{
  descriptor_pools.reset();
  immediate.reset();
}

void VKDiscardPool::deinit(VKDevice &device)
{
  destroy_discarded_resources(device);
}

void VKDiscardPool::move_data(VKDiscardPool &src_pool)
{
  std::scoped_lock mutex(mutex_);
  std::scoped_lock mutex_src(src_pool.mutex_);
  buffers_.extend(src_pool.buffers_);
  image_views_.extend(src_pool.image_views_);
  images_.extend(src_pool.images_);
  shader_modules_.extend(src_pool.shader_modules_);
  pipeline_layouts_.extend(src_pool.pipeline_layouts_);
  framebuffers_.extend(src_pool.framebuffers_);
  render_passes_.extend(src_pool.render_passes_);
  src_pool.buffers_.clear();
  src_pool.image_views_.clear();
  src_pool.images_.clear();
  src_pool.shader_modules_.clear();
  src_pool.pipeline_layouts_.clear();
  src_pool.framebuffers_.clear();
  src_pool.render_passes_.clear();
  for (const Map<VkCommandPool, Vector<VkCommandBuffer>>::Item &item :
       src_pool.command_buffers_.items())
  {
    command_buffers_.lookup_or_add_default(item.key).extend(item.value);
  }
  src_pool.command_buffers_.clear();
}

void VKDiscardPool::discard_image(VkImage vk_image, VmaAllocation vma_allocation)
{
  std::scoped_lock mutex(mutex_);
  images_.append(std::pair(vk_image, vma_allocation));
}

void VKDiscardPool::discard_command_buffer(VkCommandBuffer vk_command_buffer,
                                           VkCommandPool vk_command_pool)
{
  std::scoped_lock mutex(mutex_);
  command_buffers_.lookup_or_add_default(vk_command_pool).append(vk_command_buffer);
}

void VKDiscardPool::free_command_pool_buffers(VkCommandPool vk_command_pool, VKDevice &device)
{
  std::scoped_lock mutex(mutex_);
  std::optional<blender::Vector<VkCommandBuffer>> buffers = command_buffers_.pop_try(
      vk_command_pool);
  if (!buffers) {
    return;
  }
  vkFreeCommandBuffers(device.vk_handle(), vk_command_pool, (*buffers).size(), (*buffers).begin());
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

  Vector<uint64_t> wait_values(submit_semaphores_.size(), 1);
  VkSemaphoreWaitInfo wait_info = {};
  wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
  wait_info.semaphoreCount = submit_semaphores_.size();
  wait_info.pSemaphores = submit_semaphores_.begin();
  wait_info.pValues = wait_values.begin();
  vkWaitSemaphores(device.vk_handle(), &wait_info, UINT64_MAX);

  if (!submit_semaphores_.is_empty()) {
    BLI_assert(semaphores_guard_);
    vkWaitForFences(device.vk_handle(), 1, &semaphores_guard_, false, UINT64_MAX);
    vkDestroyFence(device.vk_handle(), semaphores_guard_, nullptr);
  }
  else {
    BLI_assert(!semaphores_guard_);
  }
  semaphores_guard_ = VK_NULL_HANDLE;

  for (auto semaphore : submit_semaphores_) {
    vkDestroySemaphore(device.vk_handle(), semaphore, nullptr);
  }
  submit_semaphores_.clear();

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

  for (const Map<VkCommandPool, Vector<VkCommandBuffer>>::Item &item : command_buffers_.items()) {
    vkFreeCommandBuffers(device.vk_handle(), item.key, item.value.size(), item.value.begin());
  }
  command_buffers_.clear();
}

SubmitSyncSemaphores VKDiscardPool::submit_sync_semaphores(VKDevice &device)
{
  std::scoped_lock mutex(mutex_);
  VkSemaphore wait_semaphore = submit_semaphores_.is_empty() ? VK_NULL_HANDLE :
                                                               submit_semaphores_.last();

  VkSemaphoreTypeCreateInfo semaphore_type_info = {};
  semaphore_type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR;
  semaphore_type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  semaphore_type_info.initialValue = 0;

  VkSemaphoreCreateInfo semaphore_info = {};
  semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphore_info.pNext = &semaphore_type_info;
  submit_semaphores_.append({});
  vkCreateSemaphore(device.vk_handle(), &semaphore_info, nullptr, &submit_semaphores_.last());
  return {wait_semaphore, submit_semaphores_.last()};
}

void VKDiscardPool::set_semaphores_fence_guard(VkFence vk_fence)
{
  std::scoped_lock mutex(mutex_);
  BLI_assert(semaphores_guard_ == VK_NULL_HANDLE);
  semaphores_guard_ = vk_fence;
}
}  // namespace blender::gpu
