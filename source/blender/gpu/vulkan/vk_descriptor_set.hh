/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_utility_mixins.hh"
#include "BLI_vector.hh"

#include "gpu_shader_private.hh"

#include "render_graph/nodes/vk_pipeline_data.hh"
#include "render_graph/vk_resource_access_info.hh"
#include "vk_buffer.hh"
#include "vk_common.hh"
#include "vk_descriptor_set_layouts.hh"
#include "vk_uniform_buffer.hh"

namespace blender::gpu {

/** Logical descriptor set indices. */
constexpr int VK_DESCRIPTOR_SET_ENGINE = 0;
constexpr int VK_DESCRIPTOR_SET_PASS = 1;
constexpr int VK_DESCRIPTOR_SET_DRAW = 2;
constexpr int VK_DESCRIPTOR_SET_NUM = 3;

struct VKResourceBinding;
class VKStateManager;
class VKDevice;
class VKPushConstants;
class VKShader;
class VKDescriptorSetTracker;
class VKVertexBuffer;

/**
 * In vulkan shader resources (images and buffers) are grouped in descriptor sets.
 *
 * The resources inside a descriptor set can be updated and bound per set.
 *
 * Blender uses 3 logical descriptor sets:
 *   - Engine  (set 0): Global resources shared across all shaders (currently empty).
 *   - Pass    (set 1): Resources that change per render pass.
 *   - Draw    (set 2): Resources that change per draw/dispatch call.
 */
class VKDescriptorSet : NonCopyable {

 public:
  /**
   * Binding location of a resource in a descriptor set.
   *
   * Locations reference both the descriptor set and the binding within that set.
   * There are 3 logical descriptor sets:
   *   - Engine  (set 0): Global resources shared across all shaders (currently empty).
   *   - Pass    (set 1): Resources that change per render pass.
   *   - Draw    (set 2): Resources that change per draw/dispatch call.
   */
  struct Location {
    friend class VKDescriptorSetTracker;
    friend class VKShaderInterface;
    friend struct VKResourceBinding;

    /**
     * Descriptor set index (Engine=0, Pass=1, Draw=2).
     */
    uint32_t set = 0;

    /**
     * Binding number within the descriptor set.
     */
    uint32_t binding = 0;

    Location(uint32_t set, uint32_t binding) : set(set), binding(binding) {}

    Location() = default;

    bool operator==(const Location &other) const
    {
      return set == other.set && binding == other.binding;
    }

    operator uint32_t() const
    {
      return binding;
    }
  };
};

class VKDescriptorSetUpdator {
 public:
  virtual ~VKDescriptorSetUpdator() {};

  virtual void allocate_new_descriptor_set(VKDevice &device,
                                           VKContext &context,
                                           VKShader &shader,
                                           VkDescriptorSetLayout vk_descriptor_set_layout) = 0;
  void bind_shader_resources(const VKDevice &device,
                             const VKStateManager &state_manager,
                             VKShader &shader,
                             int set_index,
                             const VKBufferWithOffset &push_constants_buffer);
  virtual void upload_descriptor_sets() = 0;

 private:
  void bind_image_resource(const VKStateManager &state_manager,
                           const VKResourceBinding &resource_binding);
  void bind_texture_resource(const VKDevice &device,
                             const VKStateManager &state_manager,
                             const VKResourceBinding &resource_binding);
  void bind_storage_buffer_resource(const VKStateManager &state_manager,
                                    const VKResourceBinding &resource_binding);
  void bind_uniform_buffer_resource(const VKStateManager &state_manager,
                                    const VKResourceBinding &resource_binding);
  void bind_input_attachment_resource(const VKDevice &device,
                                      const VKStateManager &state_manager,
                                      const VKResourceBinding &resource_binding);

  void bind_push_constants(VKPushConstants &push_constants,
                           const VKBufferWithOffset &push_constants_buffer);

 protected:
  virtual void bind_texel_buffer(VKVertexBuffer &vertex_buffer,
                                 VKDescriptorSet::Location location) = 0;
  virtual void bind_buffer(VkDescriptorType vk_descriptor_type,
                           VkBuffer vk_buffer,
                           VkDeviceSize buffer_offset,
                           VkDeviceSize size_in_bytes,
                           VKDescriptorSet::Location location) = 0;
  virtual void bind_image(VkDescriptorType vk_descriptor_type,
                          VkSampler vk_sampler,
                          VkImageView vk_image_view,
                          VkImageLayout vk_image_layout,
                          VKDescriptorSet::Location location) = 0;
};

class VKDescriptorSetPoolUpdator : public VKDescriptorSetUpdator {
 public:
  VkDescriptorSet vk_descriptor_set = VK_NULL_HANDLE;

  void allocate_new_descriptor_set(VKDevice &device,
                                   VKContext &context,
                                   VKShader &shader,
                                   VkDescriptorSetLayout vk_descriptor_set_layout) override;

  void upload_descriptor_sets() override;

 protected:
  void bind_texel_buffer(VKVertexBuffer &vertex_buffer,
                         VKDescriptorSet::Location location) override;
  void bind_buffer(VkDescriptorType vk_descriptor_type,
                   VkBuffer vk_buffer,
                   VkDeviceSize buffer_offset,
                   VkDeviceSize size_in_bytes,
                   VKDescriptorSet::Location location) override;
  void bind_image(VkDescriptorType vk_descriptor_type,
                  VkSampler vk_sampler,
                  VkImageView vk_image_view,
                  VkImageLayout vk_image_layout,
                  VKDescriptorSet::Location location) override;

 private:
  Vector<VkBufferView> vk_buffer_views_;
  Vector<VkDescriptorBufferInfo> vk_descriptor_buffer_infos_;
  Vector<VkDescriptorImageInfo> vk_descriptor_image_infos_;
  Vector<VkWriteDescriptorSet> vk_write_descriptor_sets_;
};

class VKDescriptorSetTracker {
  friend class VKDescriptorSet;

  /* Last used layouts to identify changes (one per set). */
  VkDescriptorSetLayout vk_descriptor_set_layouts_[VK_DESCRIPTOR_SET_NUM] = {
      VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};

 public:
  VKDescriptorSetPoolUpdator descriptor_sets_[VK_DESCRIPTOR_SET_NUM];

  VKDescriptorSetTracker() {}

  /**
   * Update the descriptor sets. Reuses previous descriptor sets when no changes are detected. This
   * improves performance when working with large grease pencil scenes.
   */
  void update_descriptor_set(VKContext &context,
                             render_graph::VKResourceAccessInfo &resource_access_info,
                             render_graph::VKPipelineData &r_pipeline_data);

  /**
   * Upload all descriptor sets to the device.
   */
  void upload_descriptor_sets();

 private:
  /**
   * Add resources of the descriptor set to the resource access info.
   */
  static void update_resource_access_info(VKContext &context,
                                          render_graph::VKResourceAccessInfo &resource_access_info,
                                          const VKBufferWithOffset &push_constants_buffer);
  static void update_resource_access_info_binding(const VKStateManager &state_manager,
                                                  const VKResourceBinding &resource_binding,
                                                  VkPipelineStageFlags vk_pipeline_stages,
                                                  render_graph::VKResourceAccessInfo &access_info);
  static void update_resource_access_info_binding_uniform_buffer(
      const VKStateManager &state_manager,
      const VKResourceBinding &resource_binding,
      VkPipelineStageFlags vk_pipeline_stages,
      render_graph::VKResourceAccessInfo &access_info);
  static void update_resource_access_info_binding_image(
      const VKStateManager &state_manager,
      const VKResourceBinding &resource_binding,
      VkPipelineStageFlags vk_pipeline_stages,
      render_graph::VKResourceAccessInfo &access_info);
  static void update_resource_access_info_binding_sampler(
      const VKStateManager &state_manager,
      const VKResourceBinding &resource_binding,
      VkPipelineStageFlags vk_pipeline_stages,
      render_graph::VKResourceAccessInfo &access_info);
  static void update_resource_access_info_binding_storage_buffer(
      const VKStateManager &state_manager,
      const VKResourceBinding &resource_binding,
      VkPipelineStageFlags vk_pipeline_stages,
      render_graph::VKResourceAccessInfo &access_info);
  static void update_resource_access_info_binding_input_attachment(
      const VKStateManager &state_manager,
      const VKResourceBinding &resource_binding,
      VkPipelineStageFlags vk_pipeline_stages,
      render_graph::VKResourceAccessInfo &access_info);
};

}  // namespace blender::gpu
