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

GHOST_XrGraphicsBindingVulkanBase::GHOST_XrGraphicsBindingVulkanBase(GHOST_Context &ghost_ctx)
    : GHOST_IXrGraphicsBinding(), m_ghost_ctx(static_cast<GHOST_ContextVK &>(ghost_ctx))
{
}

/* -------------------------------------------------------------------- */
/** \name Swapchain format.
 * \{ */

static std::optional<int64_t> choose_swapchain_format_from_candidates(
    const std::vector<int64_t> &gpu_binding_formats, const std::vector<int64_t> &runtime_formats)
{
  if (gpu_binding_formats.empty()) {
    return std::nullopt;
  }

  auto res = std::find_first_of(gpu_binding_formats.begin(),
                                gpu_binding_formats.end(),
                                runtime_formats.begin(),
                                runtime_formats.end());
  if (res == gpu_binding_formats.end()) {
    return std::nullopt;
  }

  return *res;
}

std::optional<int64_t> GHOST_XrGraphicsBindingVulkanBase::chooseSwapchainFormat(
    const std::vector<int64_t> &runtime_formats,
    GHOST_TXrSwapchainFormat &r_format,
    bool &r_is_srgb_format) const
{
  std::vector<int64_t> gpu_binding_formats = {
      VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_R8G8B8A8_SRGB,
      VK_FORMAT_B8G8R8A8_SRGB,
  };

  r_format = GHOST_kXrSwapchainFormatRGBA8;
  r_is_srgb_format = false;
  std::optional result = choose_swapchain_format_from_candidates(gpu_binding_formats,
                                                                 runtime_formats);
  if (result) {
    switch (*result) {
      case VK_FORMAT_R16G16B16A16_SFLOAT:
        r_format = GHOST_kXrSwapchainFormatRGBA16F;
        break;
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_B8G8R8A8_SRGB:
        r_format = GHOST_kXrSwapchainFormatRGBA8;
        break;
    }

    switch (*result) {
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_UNORM:
        r_is_srgb_format = false;
        break;
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_B8G8R8A8_SRGB:
        r_is_srgb_format = true;
        break;
    }
  }
  return result;
}

std::vector<XrSwapchainImageBaseHeader *> GHOST_XrGraphicsBindingVulkanBase::createSwapchainImages(
    uint32_t image_count)
{
  std::vector<XrSwapchainImageBaseHeader *> base_images;
  std::vector<XrSwapchainImageVulkanKHR> vulkan_images(
      image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, VK_NULL_HANDLE});
  for (XrSwapchainImageVulkan2KHR &image : vulkan_images) {
    base_images.push_back(reinterpret_cast<XrSwapchainImageBaseHeader *>(&image));
  }
  m_image_cache.push_back(std::move(vulkan_images));

  return base_images;
}

/* \} */

bool GHOST_XrGraphicsBindingVulkanBase::needsUpsideDownDrawing(GHOST_Context &ghost_ctx) const
{
  return ghost_ctx.isUpsideDown();
}
