/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include <sstream>

#ifdef WITH_VULKAN_BACKEND
#  include "GHOST_ContextVK.hh"
#endif

#include "GHOST_XrGraphicsBindingVulkan.hh"

/** Function pointers for Vulkan/OpenXR specific calls. */
PFN_xrGetVulkanGraphicsRequirements2KHR
    GHOST_XrGraphicsBindingVulkan::s_xrGetVulkanGraphicsRequirements2KHR_fn = nullptr;
PFN_xrGetVulkanGraphicsDevice2KHR
    GHOST_XrGraphicsBindingVulkan::s_xrGetVulkanGraphicsDevice2KHR_fn = nullptr;

bool GHOST_XrGraphicsBindingVulkan::checkVersionRequirements(GHOST_Context &ghost_ctx,
                                                             XrInstance instance,
                                                             XrSystemId system_id,
                                                             std::string *r_requirement_info) const
{
  /* Get the function pointers for OpenXR/Vulkan. If any fails we expect that we cannot use the
   * given context. */
  if (s_xrGetVulkanGraphicsRequirements2KHR_fn == nullptr &&
      XR_FAILED(
          xrGetInstanceProcAddr(instance,
                                "xrGetVulkanGraphicsRequirements2KHR",
                                (PFN_xrVoidFunction *)&s_xrGetVulkanGraphicsRequirements2KHR_fn)))
  {
    s_xrGetVulkanGraphicsRequirements2KHR_fn = nullptr;
    *r_requirement_info = std::string(
        "Unable to retrieve xrGetVulkanGraphicsRequirements2KHR instance function");
    return false;
  }
  if (s_xrGetVulkanGraphicsDevice2KHR_fn == nullptr &&
      XR_FAILED(xrGetInstanceProcAddr(instance,
                                      "xrGetVulkanGraphicsDevice2KHR",
                                      (PFN_xrVoidFunction *)&s_xrGetVulkanGraphicsDevice2KHR_fn)))
  {
    s_xrGetVulkanGraphicsDevice2KHR_fn = nullptr;
    *r_requirement_info = std::string(
        "Unable to retrieve xrGetVulkanGraphicsDevice2KHR instance function");
    return false;
  }

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

  return true;
}

void GHOST_XrGraphicsBindingVulkan::initFromGhostContext(GHOST_Context &ghost_ctx,
                                                         XrInstance instance,
                                                         XrSystemId system_id)
{
  /* Get instance handle from GHOST context. */
  VkInstance vk_instance = VK_NULL_HANDLE;
  VkQueue vk_queue = VK_NULL_HANDLE;
  VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
  VkDevice vk_device = VK_NULL_HANDLE;
  uint32_t graphics_queue_family = 0;
  void *queue_mutex = nullptr;
  ghost_ctx.getVulkanHandles(&vk_instance,
                             &vk_physical_device,
                             &vk_device,
                             &graphics_queue_family,
                             &vk_queue,
                             &queue_mutex);

  /* Get physical device from OpenXR. */
  XrVulkanGraphicsDeviceGetInfoKHR get_info = {
      XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, nullptr, system_id, vk_instance};
  VkPhysicalDevice xr_vk_physical_device = VK_NULL_HANDLE;
  s_xrGetVulkanGraphicsDevice2KHR_fn(instance, &get_info, &xr_vk_physical_device);

  oxr_binding.vk.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
  oxr_binding.vk.next = nullptr;
  oxr_binding.vk.instance = vk_instance;
  oxr_binding.vk.physicalDevice = xr_vk_physical_device;

  // TODO: we should get this one as well. Not sure how we will be doing the syncing as OpenXR
  // uses a lower level access to the GPU queues.
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
