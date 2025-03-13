/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include <sstream>

#include "GHOST_ContextVK.hh"
#include "GHOST_XrException.hh"
#include "GHOST_XrGraphicsBindingVulkan.hh"
#include "GHOST_Xr_intern.hh"

/** OpenXR/Vulkan specific function pointers. */
PFN_xrGetVulkanGraphicsRequirements2KHR
    GHOST_XrGraphicsBindingVulkan::s_xrGetVulkanGraphicsRequirements2KHR_fn = nullptr;
PFN_xrGetVulkanGraphicsDevice2KHR
    GHOST_XrGraphicsBindingVulkan::s_xrGetVulkanGraphicsDevice2KHR_fn = nullptr;
PFN_xrCreateVulkanInstanceKHR GHOST_XrGraphicsBindingVulkan::s_xrCreateVulkanInstanceKHR_fn =
    nullptr;
PFN_xrCreateVulkanDeviceKHR GHOST_XrGraphicsBindingVulkan::s_xrCreateVulkanDeviceKHR_fn = nullptr;

bool GHOST_XrGraphicsBindingVulkan::checkVersionRequirements(GHOST_Context &ghost_ctx,
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
  LOAD_PFN(s_xrGetVulkanGraphicsRequirements2KHR_fn, xrGetVulkanGraphicsRequirements2KHR);
  LOAD_PFN(s_xrGetVulkanGraphicsDevice2KHR_fn, xrGetVulkanGraphicsDevice2KHR);
  LOAD_PFN(s_xrCreateVulkanInstanceKHR_fn, xrCreateVulkanInstanceKHR);
  LOAD_PFN(s_xrCreateVulkanDeviceKHR_fn, xrCreateVulkanDeviceKHR);
#undef LOAD_PFN

  XrGraphicsRequirementsVulkanKHR xr_graphics_requirements{
      /* type */ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR,
  };
  if (XR_FAILED(s_xrGetVulkanGraphicsRequirements2KHR_fn(
          instance, system_id, &xr_graphics_requirements)))
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

  // TODO: Should we check if physical devices match? Otherwise it will be hard to blit blender
  // generated images to an openxr swapchain.

  return true;
}

void GHOST_XrGraphicsBindingVulkan::initFromGhostContext(GHOST_Context & /*ghost_ctx*/,
                                                         XrInstance instance,
                                                         XrSystemId system_id)
{
  /* Create a new VkInstance that is compatible with OpenXR */
  VkApplicationInfo vk_application_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           nullptr,
                                           "Blender",
                                           VK_MAKE_VERSION(1, 0, 0),
                                           "BlenderXR",
                                           VK_MAKE_VERSION(1, 0, 0),
                                           VK_MAKE_VERSION(1, 2, 0)};
  VkInstanceCreateInfo vk_instance_create_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                  nullptr,
                                                  0,
                                                  &vk_application_info,
                                                  0,
                                                  nullptr,
                                                  0,
                                                  nullptr};
  XrVulkanInstanceCreateInfoKHR xr_instance_create_info = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
                                                           nullptr,
                                                           system_id,
                                                           0,
                                                           vkGetInstanceProcAddr,
                                                           &vk_instance_create_info,
                                                           nullptr};
  VkResult vk_result;
  CHECK_XR(s_xrCreateVulkanInstanceKHR_fn(
               instance, &xr_instance_create_info, &m_vk_instance, &vk_result),
           "Unable to create an OpenXR compatible Vulkan instance.");

  /* Physical device selection */
  XrVulkanGraphicsDeviceGetInfoKHR xr_device_get_info = {
      XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, nullptr, system_id, m_vk_instance};
  s_xrGetVulkanGraphicsDevice2KHR_fn(instance, &xr_device_get_info, &m_vk_physical_device);

  /* Queue family */
  uint32_t vk_queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(m_vk_physical_device, &vk_queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> vk_queue_families(vk_queue_family_count);
  m_graphics_queue_family = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(
      m_vk_physical_device, &vk_queue_family_count, vk_queue_families.data());
  for (uint32_t i = 0; i < vk_queue_family_count; i++) {
    if (vk_queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT &&
        vk_queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
    {
      m_graphics_queue_family = i;
      break;
    }
  }

  /* Graphic device creation */
  const float queue_priority = 1.0f;
  VkDeviceQueueCreateInfo vk_queue_create_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                  nullptr,
                                                  0,
                                                  m_graphics_queue_family,
                                                  1,
                                                  &queue_priority};
  VkDeviceCreateInfo vk_device_create_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                              nullptr,
                                              0,
                                              1,
                                              &vk_queue_create_info,
                                              0,
                                              nullptr,
                                              0,
                                              nullptr};
  XrVulkanDeviceCreateInfoKHR xr_device_create_info = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
                                                       nullptr,
                                                       system_id,
                                                       0,
                                                       vkGetInstanceProcAddr,
                                                       m_vk_physical_device,
                                                       &vk_device_create_info,
                                                       nullptr};
  CHECK_XR(
      s_xrCreateVulkanDeviceKHR_fn(instance, &xr_device_create_info, &m_vk_device, &vk_result),
      "Unable to create an OpenXR compatible Vulkan device.");

  /* Update the binding struct */
  oxr_binding.vk.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
  oxr_binding.vk.next = nullptr;
  oxr_binding.vk.instance = m_vk_instance;
  oxr_binding.vk.physicalDevice = m_vk_physical_device;
  oxr_binding.vk.device = m_vk_device;
  oxr_binding.vk.queueFamilyIndex = m_graphics_queue_family;
  oxr_binding.vk.queueIndex = 0;
}

std::optional<int64_t> GHOST_XrGraphicsBindingVulkan::chooseSwapchainFormat(
    const std::vector<int64_t> & /*runtime_formats*/,
    GHOST_TXrSwapchainFormat & /*r_format*/,
    bool & /*r_is_srgb_format*/) const
{
  return std::nullopt;
}

std::vector<XrSwapchainImageBaseHeader *> GHOST_XrGraphicsBindingVulkan::createSwapchainImages(
    uint32_t /*image_count*/)
{
  std::vector<XrSwapchainImageBaseHeader *> base_images;

  return base_images;
}

void GHOST_XrGraphicsBindingVulkan::submitToSwapchainImage(
    XrSwapchainImageBaseHeader & /*swapchain_image*/, const GHOST_XrDrawViewInfo & /*draw_info*/)
{
}

bool GHOST_XrGraphicsBindingVulkan::needsUpsideDownDrawing(GHOST_Context &ghost_ctx) const
{
  return ghost_ctx.isUpsideDown();
}
