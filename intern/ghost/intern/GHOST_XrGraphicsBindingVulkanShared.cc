/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include <algorithm>
#include <cstring>
#include <sstream>

#include "GHOST_ContextVK.hh"
#include "GHOST_XrException.hh"
#include "GHOST_XrGraphicsBindingVulkan.hh"
#include "GHOST_Xr_intern.hh"

#ifdef _WIN32
#  include <vulkan/vulkan_win32.h>
#endif

/** OpenXR/Vulkan specific function pointers. */
PFN_xrGetVulkanGraphicsRequirementsKHR
    GHOST_XrGraphicsBindingVulkanShared::s_xrGetVulkanGraphicsRequirementsKHR_fn = nullptr;
PFN_xrGetVulkanGraphicsDeviceKHR
    GHOST_XrGraphicsBindingVulkanShared::s_xrGetVulkanGraphicsDeviceKHR_fn = nullptr;
PFN_xrGetVulkanInstanceExtensionsKHR
    GHOST_XrGraphicsBindingVulkanShared::s_xrGetVulkanInstanceExtensionsKHR = nullptr;
PFN_xrGetVulkanDeviceExtensionsKHR
    GHOST_XrGraphicsBindingVulkanShared::s_xrGetVulkanDeviceExtensionsKHR = nullptr;

GHOST_XrGraphicsBindingVulkanShared::~GHOST_XrGraphicsBindingVulkanShared()
{
  for (VkSemaphore vk_semaphore : m_vk_semaphores) {
    vkDestroySemaphore(oxr_binding.vk.device, vk_semaphore, nullptr);
  }
  m_vk_semaphores.clear();

  vkDestroyFence(oxr_binding.vk.device, m_vk_fence, nullptr);
}

bool GHOST_XrGraphicsBindingVulkanShared::checkVersionRequirements(
    GHOST_Context &ghost_ctx,
    XrInstance instance,
    XrSystemId system_id,
    std::string *r_requirement_info) const
{
#define LOAD_PFN(var, name) \
  if (var == nullptr && \
      XR_FAILED(xrGetInstanceProcAddr(instance, #name, (PFN_xrVoidFunction *)&var))) \
  { \
    var = nullptr; \
    *r_requirement_info = std::string("Unable to retrieve " #name " instance function"); \
    return false; \
  }
  /* Get the function pointers for OpenXR/Vulkan. If any fails we expect that we cannot use the
   * given context. */
  LOAD_PFN(s_xrGetVulkanGraphicsRequirementsKHR_fn, xrGetVulkanGraphicsRequirementsKHR);
  LOAD_PFN(s_xrGetVulkanGraphicsDeviceKHR_fn, xrGetVulkanGraphicsDeviceKHR);
  LOAD_PFN(s_xrGetVulkanInstanceExtensionsKHR, xrGetVulkanInstanceExtensionsKHR);
  LOAD_PFN(s_xrGetVulkanDeviceExtensionsKHR, xrGetVulkanDeviceExtensionsKHR);
#undef LOAD_PFN

  XrGraphicsRequirementsVulkanKHR xr_graphics_requirements{
      /*type*/ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR,
  };
  if (XR_FAILED(
          s_xrGetVulkanGraphicsRequirementsKHR_fn(instance, system_id, &xr_graphics_requirements)))
  {
    *r_requirement_info = std::string("Unable to retrieve Xr version requirements for Vulkan");
    return false;
  }

  /* Check if the Vulkan API instance version is supported. */
  GHOST_ContextVK &context_vk = static_cast<GHOST_ContextVK &>(ghost_ctx);
  const XrVersion vk_version = XR_MAKE_VERSION(
      context_vk.m_context_major_version, context_vk.m_context_minor_version, 0);
  if (vk_version < xr_graphics_requirements.minApiVersionSupported ||
      vk_version > xr_graphics_requirements.maxApiVersionSupported)
  {
    std::ostringstream strstream;
    strstream << "Min Vulkan version "
              << XR_VERSION_MAJOR(xr_graphics_requirements.minApiVersionSupported) << "."
              << XR_VERSION_MINOR(xr_graphics_requirements.minApiVersionSupported) << std::endl;
    strstream << "Max Vulkan version "
              << XR_VERSION_MAJOR(xr_graphics_requirements.maxApiVersionSupported) << "."
              << XR_VERSION_MINOR(xr_graphics_requirements.maxApiVersionSupported) << std::endl;

    *r_requirement_info = strstream.str();
    return false;
  }

  GHOST_VulkanHandles vulkan_handles = {};
  m_ghost_ctx.getVulkanHandles(vulkan_handles);

  VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
  if (XR_FAILED(s_xrGetVulkanGraphicsDeviceKHR_fn(
          instance, system_id, vulkan_handles.instance, &vk_physical_device)))
  {
    *r_requirement_info = std::string("Unable to retrieve Xr required physical device");
    return false;
  }

  if (vulkan_handles.physical_device != vk_physical_device) {
    *r_requirement_info = std::string("Blender requires to use the same GPU as OpenXR");
    return false;
  }

  return true;
}

void GHOST_XrGraphicsBindingVulkanShared::initFromGhostContext(GHOST_Context & /*ghost_ctx*/,
                                                               XrInstance /*instance*/,
                                                               XrSystemId /*system_id*/)
{
  GHOST_VulkanHandles vulkan_handles = {};
  m_ghost_ctx.getVulkanHandles(vulkan_handles);
  oxr_binding.vk = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
                    nullptr,
                    vulkan_handles.instance,
                    vulkan_handles.physical_device,
                    vulkan_handles.device,
                    vulkan_handles.graphic_queue_family,
                    0};
}

void GHOST_XrGraphicsBindingVulkanShared::submitToSwapchainBegin(int view_count)
{
  if (view_count > 0 && m_openxr_datas.empty()) {
    m_openxr_datas.resize(view_count, {});

    m_vk_semaphores.resize(view_count);
    for (int view_idx = 0; view_idx < view_count; view_idx++) {
      VkSemaphoreCreateInfo vk_semaphore_create_info = {
          VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
      vkCreateSemaphore(
          oxr_binding.vk.device, &vk_semaphore_create_info, nullptr, &m_vk_semaphores[view_idx]);
    }
    VkFenceCreateInfo vk_fence_create_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    vkCreateFence(oxr_binding.vk.device, &vk_fence_create_info, nullptr, &m_vk_fence);
  }
}

void GHOST_XrGraphicsBindingVulkanShared::submitToSwapchainImage(
    XrSwapchainImageBaseHeader &swapchain_image, const GHOST_XrDrawViewInfo &draw_info)
{
  XrSwapchainImageVulkan2KHR &vulkan_image = *reinterpret_cast<XrSwapchainImageVulkan2KHR *>(
      &swapchain_image);

  GHOST_VulkanOpenXRData &openxr_data = m_openxr_datas[draw_info.view_idx];
  openxr_data.data_transfer_mode = GHOST_kVulkanXRModeShared;
  openxr_data.shared.view_offset = {draw_info.ofsx, draw_info.ofsy};
  openxr_data.shared.xr_swapchain_image = vulkan_image.image;
  openxr_data.shared.xr_wait_semaphore = draw_info.is_first_view ?
                                             VK_NULL_HANDLE :
                                             m_vk_semaphores[draw_info.view_idx - 1];
  openxr_data.shared.xr_signal_semaphore = draw_info.is_last_view ?
                                               VK_NULL_HANDLE :
                                               m_vk_semaphores[draw_info.view_idx];
  openxr_data.shared.xr_fence = draw_info.is_last_view ? m_vk_fence : VK_NULL_HANDLE;

  m_ghost_ctx.openxr_acquire_framebuffer_image_callback_(&openxr_data);
}

void GHOST_XrGraphicsBindingVulkanShared::submitToSwapchainEnd()
{
  vkWaitForFences(oxr_binding.vk.device, 1, &m_vk_fence, false, UINT64_MAX);
  vkResetFences(oxr_binding.vk.device, 1, &m_vk_fence);
  for (GHOST_VulkanOpenXRData &openxr_data : m_openxr_datas) {
    m_ghost_ctx.openxr_release_framebuffer_image_callback_(&openxr_data);
  }
}
