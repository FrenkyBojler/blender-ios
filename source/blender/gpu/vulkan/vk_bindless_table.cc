/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_bindless_table.hh"
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

void VKBindlessTable::debug_print() const
{
  std::cout << "Global Bindless Table: \n";
  std::cout << "\tStorage Buffers: \n";
  for (auto elem : bound_storage_buffers_.items()) {
    std::cout << "\t\tStorage Buffer: " << elem.key << ", Slot: " << elem.value << "\n";
  }
  std::cout << "\tStorage Images: \n";
  for (auto elem : bound_storage_images_.items()) {
    std::cout << "\t\tImage View: " << elem.key.imageView << ", Slot: " << elem.value << "\n";
  }
  std::cout << "\tCombined Image Samplers: \n";
  for (auto elem : bound_combined_image_samplers_.items()) {
    std::cout << "\t\tImage View: " << elem.key.imageView << ", Slot: " << elem.value << "\n";
  }
  std::cout << "\tUniform Buffers: \n";
  for (auto elem : bound_uniform_buffers_.items()) {
    std::cout << "\t\tUniform Buffer: " << elem.key << ", Slot: " << elem.value << "\n";
  }
  std::cout << "\tUniform Texel Buffers: \n";
  for (auto elem : bound_uniform_texel_buffers_.items()) {
    std::cout << "\t\tBuffer View: " << elem.key << ", Slot: " << elem.value << "\n";
  }
}

DescriptorSlot VKBindlessTable::addStorageBuffer(VkBuffer buffer_handle, VkDeviceSize buffer_size)
{
  std::scoped_lock lock(mutex_);

  if (bound_storage_buffers_.contains(buffer_handle)) {
    return bound_storage_buffers_.lookup(buffer_handle);
  }

  BLI_assert(descriptor_set != VK_NULL_HANDLE);

  DescriptorSlot slot = storage_buffer_free_slots_.pop_last();

  bound_storage_buffers_.add(buffer_handle, slot);

  return slot;
}

bool VKBindlessTable::isStorageBufferBound(VkBuffer buffer_handle)
{
  return bound_storage_buffers_.contains(buffer_handle);
}

DescriptorSlot VKBindlessTable::getSlotForBuffer(VkBuffer buffer_handle)
{
  std::scoped_lock lock(mutex_);
  BLI_assert(bound_storage_buffers_.contains(buffer_handle));

  return bound_storage_buffers_.lookup(buffer_handle);
}

void VKBindlessTable::removeStorageBuffer(VkBuffer buffer)
{
  std::scoped_lock lock(mutex_);

  if (bound_storage_buffers_.contains(buffer)) {
    DescriptorSlot slot = bound_storage_buffers_.pop(buffer);
    storage_buffer_free_slots_.append(slot);
  }
}

DescriptorSlot VKBindlessTable::addUniform(VkBuffer buffer_handle, VkDeviceSize buffer_size)
{
  std::scoped_lock lock(mutex_);

  if (bound_uniform_buffers_.contains(buffer_handle)) {
    return bound_uniform_buffers_.lookup(buffer_handle);
  }

  BLI_assert(descriptor_set != VK_NULL_HANDLE);

  DescriptorSlot slot = uniform_buffer_free_slots_.pop_last();

  bound_uniform_buffers_.add(buffer_handle, slot);

  return slot;
}

DescriptorSlot VKBindlessTable::getSlotForUniform(VkBuffer buffer_handle)
{
  std::scoped_lock lock(mutex_);
  BLI_assert(bound_uniform_buffers_.contains(buffer_handle));

  return bound_uniform_buffers_.lookup(buffer_handle);
}

bool VKBindlessTable::isUniformBufferBound(VkBuffer buffer_handle)
{
  return bound_uniform_buffers_.contains(buffer_handle);
}

void VKBindlessTable::removeUniform(VkBuffer buffer)
{
  std::scoped_lock lock(mutex_);

  if (bound_uniform_buffers_.contains(buffer)) {
    DescriptorSlot slot = bound_uniform_buffers_.pop(buffer);
    uniform_buffer_free_slots_.append(slot);
  }
}

DescriptorSlot VKBindlessTable::addImage(VkDescriptorImageInfo info)
{
  std::scoped_lock lock(mutex_);

  BLI_assert(descriptor_set != VK_NULL_HANDLE);

  if (bound_combined_image_samplers_.contains(info)) {
    return bound_combined_image_samplers_.lookup(info);
  }

  DescriptorSlot slot = combined_image_sampler_free_slots_.pop_last();

  bound_combined_image_samplers_.add(info, slot);
  image_view_to_combined_image_sampler_keys_.lookup_or_add_default(info.imageView).append(info);

  return slot;
}

DescriptorSlot VKBindlessTable::getSlotForImage(VkDescriptorImageInfo info)
{
  std::scoped_lock lock(mutex_);
  BLI_assert(bound_combined_image_samplers_.contains(info));

  return bound_combined_image_samplers_.lookup(info);
}

bool VKBindlessTable::isImageBound(VkDescriptorImageInfo info)
{
  return bound_combined_image_samplers_.contains(info);
}

void VKBindlessTable::removeAllWithImageView(VkImageView image_view)
{
  std::scoped_lock lock(mutex_);

  if (image_view_to_combined_image_sampler_keys_.contains(image_view)) {
    for (VkDescriptorImageInfo &info :
         image_view_to_combined_image_sampler_keys_.lookup(image_view))
    {
      DescriptorSlot slot = bound_combined_image_samplers_.pop(info);
      combined_image_sampler_free_slots_.append(slot);
    }

    image_view_to_combined_image_sampler_keys_.remove(image_view);
  }

  if (image_view_to_storage_image_keys_.contains(image_view)) {
    for (VkDescriptorImageInfo &info : image_view_to_storage_image_keys_.lookup(image_view)) {
      DescriptorSlot slot = bound_storage_images_.pop(info);
      storage_image_free_slots_.append(slot);
    }

    image_view_to_storage_image_keys_.remove(image_view);
  }
}

void VKBindlessTable::removeImage(VkDescriptorImageInfo info)
{
  std::scoped_lock lock(mutex_);

  if (bound_combined_image_samplers_.contains(info)) {
    DescriptorSlot slot = bound_combined_image_samplers_.pop(info);
    combined_image_sampler_free_slots_.append(slot);
  }
}

DescriptorSlot VKBindlessTable::addStorageImage(VkDescriptorImageInfo info)
{
  std::scoped_lock lock(mutex_);

  BLI_assert(descriptor_set != VK_NULL_HANDLE);

  if (bound_storage_images_.contains(info)) {
    return bound_storage_images_.lookup(info);
  }

  DescriptorSlot slot = storage_image_free_slots_.pop_last();

  bound_storage_images_.add(info, slot);
  image_view_to_storage_image_keys_.lookup_or_add_default(info.imageView).append(info);

  return slot;
}

DescriptorSlot VKBindlessTable::getSlotForStorageImage(VkDescriptorImageInfo info)
{
  std::scoped_lock lock(mutex_);
  BLI_assert(bound_storage_images_.contains(info));

  return bound_storage_images_.lookup(info);
}

bool VKBindlessTable::isStorageImageBound(VkDescriptorImageInfo info)
{
  return bound_storage_images_.contains(info);
}

void VKBindlessTable::removeStorageImagesWithImageView(VkImageView image_view)
{
  std::scoped_lock lock(mutex_);

  if (!image_view_to_storage_image_keys_.contains(image_view)) {
    return;
  }

  for (VkDescriptorImageInfo info : image_view_to_storage_image_keys_.lookup(image_view)) {
    DescriptorSlot slot = bound_storage_images_.pop(info);
    storage_image_free_slots_.append(slot);
  }

  image_view_to_storage_image_keys_.remove(image_view);
}

void VKBindlessTable::removeStorageImage(VkDescriptorImageInfo info)
{
  std::scoped_lock lock(mutex_);

  if (bound_storage_images_.contains(info)) {
    DescriptorSlot slot = bound_storage_images_.pop(info);
    storage_image_free_slots_.append(slot);
  }
}

DescriptorSlot VKBindlessTable::addTexelBuffer(VkBufferView buffer_view)
{
  std::scoped_lock lock(mutex_);

  if (bound_uniform_texel_buffers_.contains(buffer_view)) {
    return bound_uniform_texel_buffers_.lookup(buffer_view);
  }

  BLI_assert(descriptor_set != VK_NULL_HANDLE);

  DescriptorSlot slot = uniform_texel_buffer_free_slots_.pop_last();

  bound_uniform_texel_buffers_.add(buffer_view, slot);

  return slot;
}

DescriptorSlot VKBindlessTable::getSlotForTexelBuffer(VkBufferView buffer_view)
{
  std::scoped_lock lock(mutex_);
  BLI_assert(bound_uniform_texel_buffers_.contains(buffer_view));

  return bound_uniform_texel_buffers_.lookup(buffer_view);
}

bool VKBindlessTable::isTexelBufferBound(VkBufferView view)
{
  return bound_uniform_texel_buffers_.contains(view);
}

void VKBindlessTable::removeTexelBuffer(VkBufferView buffer_view)
{
  std::scoped_lock lock(mutex_);

  if (bound_uniform_texel_buffers_.contains(buffer_view)) {
    DescriptorSlot slot = bound_uniform_texel_buffers_.pop(buffer_view);
    uniform_texel_buffer_free_slots_.append(slot);
  }
}

}  // namespace blender::gpu
