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
    GHOST_XrGraphicsBindingVulkanShared::s_xrGetVulkanInstanceExtensionsKHR_fn = nullptr;
PFN_xrGetVulkanDeviceExtensionsKHR
    GHOST_XrGraphicsBindingVulkanShared::s_xrGetVulkanDeviceExtensionsKHR_fn = nullptr;

GHOST_XrGraphicsBindingVulkanShared::~GHOST_XrGraphicsBindingVulkanShared()
{
  if (m_vk_fence != VK_NULL_HANDLE) {
    vkDestroyFence(oxr_binding.vk.device, m_vk_fence, nullptr);
  }
}

static std::vector<std::string> split_by_space(std::string text)
{
  std::string line;
  std::vector<std::string> vec;
  std::stringstream ss(text);
  while (std::getline(ss, line, ' ')) {
    vec.push_back(line);
  }
  return vec;
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
  LOAD_PFN(s_xrGetVulkanInstanceExtensionsKHR_fn, xrGetVulkanInstanceExtensionsKHR);
  LOAD_PFN(s_xrGetVulkanDeviceExtensionsKHR_fn, xrGetVulkanDeviceExtensionsKHR);
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

  /* Read the required instance extensions. */
  uint32_t buffer_count = 0;
  if (XR_FAILED(
          s_xrGetVulkanInstanceExtensionsKHR_fn(instance, system_id, 0, &buffer_count, nullptr)))
  {
    *r_requirement_info = std::string("Unable to determine required instance vulkan extensions");
    return false;
  }
  char *buffer = static_cast<char *>(malloc(buffer_count));
  if (XR_FAILED(s_xrGetVulkanInstanceExtensionsKHR_fn(
          instance, system_id, buffer_count, &buffer_count, buffer)))
  {
    *r_requirement_info = std::string("Unable to determine required instance vulkan extensions");
    free(buffer);
    return false;
  }
  std::vector<std::string> instance_extensions = split_by_space(buffer);
  free(buffer);
  buffer = nullptr;
  buffer_count = 0;

  /* Read the required device extensions. */
  if (XR_FAILED(
          s_xrGetVulkanDeviceExtensionsKHR_fn(instance, system_id, 0, &buffer_count, nullptr)))
  {
    *r_requirement_info = std::string("Unable to determine required device vulkan extensions");
    return false;
  }
  buffer = static_cast<char *>(malloc(buffer_count));
  if (XR_FAILED(s_xrGetVulkanDeviceExtensionsKHR_fn(
          instance, system_id, buffer_count, &buffer_count, buffer)))
  {
    *r_requirement_info = std::string("Unable to determine required device vulkan extensions");
    free(buffer);
    return false;
  }
  std::vector<std::string> device_extensions = split_by_space(buffer);
  free(buffer);
  buffer = nullptr;

  std::vector<std::string> missing_extensions;
  /* Check for enabled instance extensions. */
  for (const std::string &extension : instance_extensions) {
    if (!context_vk.is_instance_extension_enabled(extension)) {
      missing_extensions.push_back(extension);
    }
  }

  /* Check for enabled instance extensions. */
  for (const std::string &extension : device_extensions) {
    if (!context_vk.is_device_extension_enabled(extension)) {
      missing_extensions.push_back(extension);
    }
  }
  if (!missing_extensions.empty()) {
    std::stringstream ss;
    ss << "Unable to use shared resources as extensions aren't enabled: [";
    for (std::string &extension : missing_extensions) {
      ss << extension << " ";
    }
    ss << "]";
    *r_requirement_info = ss.str();
    return false;
  }

  GHOST_VulkanHandles vulkan_handles = {};
  m_ghost_ctx.getVulkanHandles(vulkan_handles);

  VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
  XrResult xr_result = s_xrGetVulkanGraphicsDeviceKHR_fn(
      instance, system_id, vulkan_handles.instance, &vk_physical_device);
  if (XR_FAILED(xr_result)) {
    std::stringstream ss;
    ss << "Unable to retrieve Xr required physical device. xr_result=" << xr_result
       << ", system_id=" << system_id << ", vk_instance=" << vulkan_handles.instance << "\n";
    *r_requirement_info = ss.str();
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

  VkFenceCreateInfo vk_fence_create_info = {
      VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
  vkCreateFence(oxr_binding.vk.device, &vk_fence_create_info, nullptr, &m_vk_fence);
}

void GHOST_XrGraphicsBindingVulkanShared::submitToSwapchainBegin(int view_count)
{
  if (view_count > 0 && m_openxr_datas.empty()) {
    m_openxr_datas.resize(view_count, {});
  }
  vkResetFences(oxr_binding.vk.device, 1, &m_vk_fence);
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
  openxr_data.shared.xr_fence = draw_info.is_last_view ? m_vk_fence : VK_NULL_HANDLE;

  m_ghost_ctx.openxr_acquire_framebuffer_image_callback_(&openxr_data);
}

void GHOST_XrGraphicsBindingVulkanShared::submitToSwapchainEnd()
{
  vkWaitForFences(oxr_binding.vk.device, 1, &m_vk_fence, false, UINT64_MAX);
  for (GHOST_VulkanOpenXRData &openxr_data : m_openxr_datas) {
    m_ghost_ctx.openxr_release_framebuffer_image_callback_(&openxr_data);
  }
}
