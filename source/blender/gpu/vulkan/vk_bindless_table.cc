/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_bindless_table.hh"
#include "BLI_assert.h"
#include "BLI_utildefines.h"
#include "vk_backend.hh"
#include "vk_buffer.hh"
#include <mutex>
#include <vulkan/vulkan_core.h>

namespace blender::gpu {

void VKBindlessTable::init()
{
  const VKDevice &device = VKBackend::get().device;

  // Create the layout for our single descriptor set.
  {
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    std::array<VkDescriptorBindingFlags, 5> flags{};
    std::array<VkDescriptorType, 5> types = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
    };

    for (uint32_t i = 0; i < 5; i++) {
      bindings[i].binding = i;
      bindings[i].descriptorType = types[i];
      bindings[i].descriptorCount = num_descriptors_per_resource;
      bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
      flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                 VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{};
    bindingFlags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlags.pNext = nullptr;
    bindingFlags.pBindingFlags = flags.data();
    bindingFlags.bindingCount = 5;

    VkDescriptorSetLayoutCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.bindingCount = 5;
    create_info.pBindings = bindings.data();
    create_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    create_info.pNext = &bindingFlags;

    VkResult result = vkCreateDescriptorSetLayout(
        device.vk_handle(), &create_info, nullptr, &descriptor_set_layout);
    BLI_assert(result == VK_SUCCESS);
    UNUSED_VARS(result);
  }

  // Create descriptor pool that will manage memory for our single
  // bindless descriptor set.
  {
    Vector<VkDescriptorPoolSize> pool_sizes = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, num_descriptors_per_resource},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, num_descriptors_per_resource},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, num_descriptors_per_resource},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, num_descriptors_per_resource},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, num_descriptors_per_resource},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, num_descriptors_per_resource}};

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = pool_sizes.size();
    pool_info.pPoolSizes = pool_sizes.data();
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    VkResult result = vkCreateDescriptorPool(
        device.vk_handle(), &pool_info, nullptr, &descriptor_pool_);
    BLI_assert(result == VK_SUCCESS);
    UNUSED_VARS(result);
  }

  // Now create our single descriptor set.
  {
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = descriptor_pool_;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &descriptor_set_layout;
    VkResult result = vkAllocateDescriptorSets(
        device.vk_handle(), &allocate_info, &descriptor_set);
    BLI_assert(result == VK_SUCCESS);
    UNUSED_VARS(result);
  }
}

void VKBindlessTable::free()
{
  VKDevice &device = VKBackend::get().device;

  vkDestroyDescriptorPool(device.vk_handle(), descriptor_pool_, nullptr);
  vkDestroyDescriptorSetLayout(device.vk_handle(), descriptor_set_layout, nullptr);
}

DescriptorSlot VKBindlessTable::provision_storage_buffer()
{
  std::scoped_lock lock(mutex_);

  DescriptorSlot slot = storage_buffer_free_slots_.pop_last();
  return slot;
}

DescriptorSlot VKBindlessTable::provision_uniform_buffer()
{
  std::scoped_lock lock(mutex_);

  DescriptorSlot slot = uniform_buffer_free_slots_.pop_last();
  return slot;
}

DescriptorSlot VKBindlessTable::provision_combined_image_sampler()
{
  std::scoped_lock lock(mutex_);

  DescriptorSlot slot = combined_image_sampler_free_slots_.pop_last();
  return slot;
}

DescriptorSlot VKBindlessTable::provision_storage_image()
{
  std::scoped_lock lock(mutex_);

  DescriptorSlot slot = storage_image_free_slots_.pop_last();
  return slot;
}

DescriptorSlot VKBindlessTable::provision_texel_buffer()
{
  std::scoped_lock lock(mutex_);

  DescriptorSlot slot = uniform_texel_buffer_free_slots_.pop_last();
  return slot;
}

void VKBindlessTable::free_binding(VKGlobalDescriptorBinding descriptor_binding)
{
  std::scoped_lock lock(mutex_);

  switch (descriptor_binding.descriptor_type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      uniform_buffer_free_slots_.append(descriptor_binding.descriptor_slot);
      break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      storage_buffer_free_slots_.append(descriptor_binding.descriptor_slot);
      break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      combined_image_sampler_free_slots_.append(descriptor_binding.descriptor_slot);
      break;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      storage_image_free_slots_.append(descriptor_binding.descriptor_slot);
      break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      uniform_texel_buffer_free_slots_.append(descriptor_binding.descriptor_slot);
      break;
    default:
      BLI_assert_unreachable();
  }
}

}  // namespace blender::gpu
