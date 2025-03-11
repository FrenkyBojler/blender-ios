/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_IXrGraphicsBinding.hh"
#include "GHOST_Types.h"

class GHOST_XrGraphicsBindingVulkan : public GHOST_IXrGraphicsBinding {
 public:
  /**
   * Check the version requirements to use OpenXR with the Vulkan backend.
   *
   * - Physical device should match. GHOST assumes that there is only one device active. When they
   * don't match user is reported with the device that would work. User can select the device from
   * the preferences.
   * - API version is compatible.
   */
  bool checkVersionRequirements(GHOST_Context &ghost_ctx,
                                XrInstance instance,
                                XrSystemId system_id,
                                std::string *r_requirement_info) const override;

  void initFromGhostContext(GHOST_Context &ghost_ctx,
                            XrInstance instance,
                            XrSystemId system_id) override;
  std::optional<int64_t> chooseSwapchainFormat(const std::vector<int64_t> &runtime_formats,
                                               GHOST_TXrSwapchainFormat &r_format,
                                               bool &r_is_srgb_format) const override;
  std::vector<XrSwapchainImageBaseHeader *> createSwapchainImages(uint32_t image_count) override;

  void submitToSwapchainImage(XrSwapchainImageBaseHeader &swapchain_image,
                              const GHOST_XrDrawViewInfo &draw_info) override;

  bool needsUpsideDownDrawing(GHOST_Context &ghost_ctx) const override;

 private:
  static PFN_xrGetVulkanGraphicsRequirements2KHR s_xrGetVulkanGraphicsRequirements2KHR_fn;
  static PFN_xrGetVulkanGraphicsDevice2KHR s_xrGetVulkanGraphicsDevice2KHR_fn;
};
