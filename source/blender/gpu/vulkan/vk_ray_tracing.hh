#pragma once

#include "BLI_vector.hh"

#include "GPU_ray_tracing.hh"

#include "vk_buffer.hh"
#include "vk_common.hh"

#include "render_graph/nodes/vk_build_acceleration_structure_node.hh"

namespace blender::gpu {

class VKTopLevelAS : public TopLevelAS {
  /** Handle of the acceleration structure. */
  VkAccelerationStructureKHR vk_acceleration_structure_ = VK_NULL_HANDLE;
  VkDeviceAddress vk_device_address_ = 0;
  uint32_t max_primitive_count_;

  /** Backing buffer for the VkAccelerationStructure handle.  */
  VKBuffer buffer_;
  /** Create info for building the acceleration structure using the render graph.  */
  render_graph::VKBuildAccelerationStructureNode::CreateInfo build_acceleration_structure_info_ =
      {};

  Vector<VkAccelerationStructureInstanceKHR> instances_;
  VKBuffer instances_buffer_;
  bool is_dirty_ = true;

 public:
  VKTopLevelAS(const char *name);
  ~VKTopLevelAS();
  std::optional<InstanceID> add_instance(const BottomLevelAS &blas,
                                         const float4x4 &mat,
                                         uint8_t mask) override;
  bool update_instance(InstanceID instance_id, const float4x4 &mat, uint8_t mask) override;
  bool build() override;
  bool bind(int slot) override;

  VkAccelerationStructureKHR vk_acceleration_structure() const
  {
    BLI_assert(vk_acceleration_structure_ != VK_NULL_HANDLE);
    return vk_acceleration_structure_;
  }

  VkBuffer vk_buffer() const
  {
    return buffer_.vk_handle();
  }

  VkDeviceAddress vk_device_address() const
  {
    BLI_assert(vk_device_address_ != 0);
    return vk_device_address_;
  }
};

class VKBottomLevelAS : public BottomLevelAS {
  /** Handle of the acceleration structure. */
  VkAccelerationStructureKHR vk_acceleration_structure_ = VK_NULL_HANDLE;
  VkDeviceAddress vk_device_address_ = 0;
  Vector<uint32_t> max_primitive_count_per_geometry_;

  /** Backing buffer for the VkAccelerationStructure handle.  */
  VKBuffer buffer_;
  /** Create info for building the acceleration structure using the render graph.  */
  render_graph::VKBuildAccelerationStructureNode::CreateInfo build_acceleration_structure_info_ =
      {};

 public:
  VKBottomLevelAS(const char *name);
  ~VKBottomLevelAS();

  bool add_geometry(IndexBuf &index_buffer, VertBuf &vertex_buffer) override;
  bool build() override;

  VkAccelerationStructureKHR vk_acceleration_structure() const
  {
    BLI_assert(vk_acceleration_structure_ != VK_NULL_HANDLE);
    return vk_acceleration_structure_;
  }

  VkBuffer vk_buffer() const
  {
    return buffer_.vk_handle();
  }

  VkDeviceAddress vk_device_address() const
  {
    BLI_assert(vk_device_address_ != 0);
    return vk_device_address_;
  }
};

static inline const VKTopLevelAS &unwrap(const TopLevelAS &blas)
{
  return static_cast<const VKTopLevelAS &>(blas);
}
static inline const VKBottomLevelAS &unwrap(const BottomLevelAS &blas)
{
  return static_cast<const VKBottomLevelAS &>(blas);
}
}  // namespace blender::gpu
