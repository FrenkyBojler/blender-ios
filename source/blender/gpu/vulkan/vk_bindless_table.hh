/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_mutex.hh"
#include "BLI_utility_mixins.hh"
#include "BLI_vector.hh"
#include "vk_common.hh"

namespace blender {

/**
 * Default hash for VkDescriptorImageInfo.
 */
template<> struct DefaultHash<VkDescriptorImageInfo> {
  uint64_t operator()(const VkDescriptorImageInfo &key) const
  {
    uint64_t hash = uint64_t(key.imageLayout);
    hash = hash * 33 ^ uint64_t(key.imageView);
    hash = hash * 33 ^ uint64_t(key.sampler);
    return hash;
  }
};

template<> struct DefaultEquality<VkDescriptorImageInfo> {
  bool operator()(const VkDescriptorImageInfo &a, const VkDescriptorImageInfo &b) const
  {
    return (a.imageLayout == b.imageLayout) && (a.imageView == b.imageView) &&
           (a.sampler == b.sampler);
  }
};

}  // namespace blender

namespace blender::gpu {

using DescriptorSlot = uint32_t;

struct VKGlobalDescriptorBinding {
  VkDescriptorType descriptor_type;
  DescriptorSlot descriptor_slot;
};

/**
 * Table mapping resources to descriptors in a bindless descriptor set.
 */
class VKBindlessTable : NonCopyable {
  static constexpr uint32_t num_descriptors_per_resource = 10000;

  Mutex mutex_;

  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

  Vector<DescriptorSlot> storage_buffer_free_slots_;

  Vector<DescriptorSlot> uniform_buffer_free_slots_;

  Vector<DescriptorSlot> combined_image_sampler_free_slots_;
  Map<VkDescriptorImageInfo, DescriptorSlot> bound_combined_image_samplers_;
  Map<VkImageView, Vector<VkDescriptorImageInfo>> image_view_to_combined_image_sampler_keys_;

  Vector<DescriptorSlot> storage_image_free_slots_;
  Map<VkDescriptorImageInfo, DescriptorSlot> bound_storage_images_;
  Map<VkImageView, Vector<VkDescriptorImageInfo>> image_view_to_storage_image_keys_;

  Vector<DescriptorSlot> uniform_texel_buffer_free_slots_;
  Map<VkBufferView, DescriptorSlot> bound_uniform_texel_buffers_;

 public:
  static constexpr uint32_t storage_buffer_binding = 0;
  static constexpr uint32_t uniform_binding = 1;
  static constexpr uint32_t combined_image_sampler_binding = 2;
  static constexpr uint32_t storage_image_binding = 3;
  static constexpr uint32_t uniform_texel_buffer_binding = 4;

  VKBindlessTable()
  {
    for (uint32_t i = 0; i < num_descriptors_per_resource; i++) {
      storage_buffer_free_slots_.append(num_descriptors_per_resource - i - 1);
      uniform_buffer_free_slots_.append(num_descriptors_per_resource - i - 1);
      combined_image_sampler_free_slots_.append(num_descriptors_per_resource - i - 1);
      storage_image_free_slots_.append(num_descriptors_per_resource - i - 1);
      uniform_texel_buffer_free_slots_.append(num_descriptors_per_resource - i - 1);
    }
  }

  void init();
  void free();

  VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

  DescriptorSlot provision_storage_buffer();

  DescriptorSlot provision_uniform_buffer();

  DescriptorSlot provision_combined_image_sampler();

  DescriptorSlot provision_storage_image();

  DescriptorSlot provision_texel_buffer();

  void free_binding(VKGlobalDescriptorBinding descriptor_binding);
};

}  // namespace blender::gpu
