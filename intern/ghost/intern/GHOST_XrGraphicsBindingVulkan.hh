/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#pragma once

#include <list>

#define VMA_VULKAN_VERSION 1002000  // Vulkan 1.2
#include "vk_mem_alloc.h"

#include "GHOST_ContextVK.hh"
#include "GHOST_IXrGraphicsBinding.hh"
#include "GHOST_Types.h"

class GHOST_XrGraphicsBindingVulkanBase : public GHOST_IXrGraphicsBinding {
 public:
  GHOST_XrGraphicsBindingVulkanBase(GHOST_Context &ghost_ctx);

  std::optional<int64_t> chooseSwapchainFormat(const std::vector<int64_t> &runtime_formats,
                                               GHOST_TXrSwapchainFormat &r_format,
                                               bool &r_is_srgb_format) const override;
  std::vector<XrSwapchainImageBaseHeader *> createSwapchainImages(uint32_t image_count) override;
  bool needsUpsideDownDrawing(GHOST_Context &ghost_ctx) const override;

 protected:
  GHOST_ContextVK &m_ghost_ctx;

 private:
  std::list<std::vector<XrSwapchainImageVulkan2KHR>> m_image_cache;
};

/**
 * Vulkan graphics binding where OpenXR has its own logical device and external
 * memory/semaphores/fence are used to sync the GPU work.
 *
 * This decreases performance due to the many steps involved to sync the images.
 */
class GHOST_XrGraphicsBindingVulkan : public GHOST_XrGraphicsBindingVulkanBase {
 public:
  GHOST_XrGraphicsBindingVulkan(GHOST_Context &ghost_ctx)
      : GHOST_XrGraphicsBindingVulkanBase(ghost_ctx)
  {
  }

  ~GHOST_XrGraphicsBindingVulkan() override;

  /**
   * Check the version requirements to use OpenXR with the Vulkan backend.
   */
  bool checkVersionRequirements(GHOST_Context &ghost_ctx,
                                XrInstance instance,
                                XrSystemId system_id,
                                std::string *r_requirement_info) const override;

  void initFromGhostContext(GHOST_Context &ghost_ctx,
                            XrInstance instance,
                            XrSystemId system_id) override;

  void submitToSwapchainBegin(int view_count) override;
  void submitToSwapchainImage(XrSwapchainImageBaseHeader &swapchain_image,
                              const GHOST_XrDrawViewInfo &draw_info) override;
  void submitToSwapchainEnd() override;

  XrSystemId getSystemId() override
  {
    return XR_NULL_SYSTEM_ID;
  }

 private:
  VkInstance m_vk_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_vk_physical_device = VK_NULL_HANDLE;
  uint32_t m_graphics_queue_family = 0;
  VkQueue m_vk_queue = VK_NULL_HANDLE;
  VkDevice m_vk_device = VK_NULL_HANDLE;
  VmaAllocator m_vma_allocator = VK_NULL_HANDLE;
  VmaAllocation m_vk_buffer_allocation = VK_NULL_HANDLE;
  VkBuffer m_vk_buffer = VK_NULL_HANDLE;
  VmaAllocationInfo m_vk_buffer_allocation_info = {};
  GHOST_TVulkanXRModes m_data_transfer_mode = GHOST_kVulkanXRModeCPU;

  VkCommandPool m_vk_command_pool = VK_NULL_HANDLE;

  struct ImportedMemory {
    char view_idx;
    VkImage vk_image_blender;
    VkImage vk_image_xr;
    VkDeviceMemory vk_device_memory_xr;
  };
  std::vector<ImportedMemory> m_imported_memory;

  GHOST_TVulkanXRModes choseDataTransferMode();
  void submitToSwapchainImageCpu(XrSwapchainImageVulkan2KHR &swapchain_image,
                                 const GHOST_XrDrawViewInfo &draw_info);
  void submitToSwapchainImageGpu(XrSwapchainImageVulkan2KHR &swapchain_image,
                                 const GHOST_XrDrawViewInfo &draw_info);

  /**
   * Single VkCommandBuffer that is used for all views/swap-chains.
   *
   * This can be improved by having a single command buffer per swap-chain image.
   */
  VkCommandBuffer m_vk_command_buffer = VK_NULL_HANDLE;

  static PFN_xrGetVulkanGraphicsRequirements2KHR s_xrGetVulkanGraphicsRequirements2KHR_fn;
  static PFN_xrGetVulkanGraphicsDevice2KHR s_xrGetVulkanGraphicsDevice2KHR_fn;
  static PFN_xrCreateVulkanInstanceKHR s_xrCreateVulkanInstanceKHR_fn;
  static PFN_xrCreateVulkanDeviceKHR s_xrCreateVulkanDeviceKHR_fn;
};

class GHOST_XrGraphicsBindingVulkanShared : public GHOST_XrGraphicsBindingVulkanBase {
 public:
  GHOST_XrGraphicsBindingVulkanShared(GHOST_Context &ghost_ctx)
      : GHOST_XrGraphicsBindingVulkanBase(ghost_ctx)
  {
  }

  ~GHOST_XrGraphicsBindingVulkanShared() override;

  /**
   * Check the version requirements to use OpenXR with the Vulkan backend.
   */
  bool checkVersionRequirements(GHOST_Context &ghost_ctx,
                                XrInstance instance,
                                XrSystemId system_id,
                                std::string *r_requirement_info) const override;

  void initFromGhostContext(GHOST_Context &ghost_ctx,
                            XrInstance instance,
                            XrSystemId system_id) override;

  void submitToSwapchainBegin(int view_count) override;
  void submitToSwapchainImage(XrSwapchainImageBaseHeader &swapchain_image,
                              const GHOST_XrDrawViewInfo &draw_info) override;
  void submitToSwapchainEnd() override;
  XrSystemId getSystemId() override;

 private:
  /**
   * Fence to signal by the last swap chain image. This will inform OpenXR to proceed and temp
   * resources can be freed.
   */
  VkFence m_vk_fence = VK_NULL_HANDLE;
  std::vector<GHOST_VulkanOpenXRData> m_openxr_datas;

  static PFN_xrGetVulkanGraphicsRequirements2KHR s_xrGetVulkanGraphicsRequirements2KHR_fn;
};
