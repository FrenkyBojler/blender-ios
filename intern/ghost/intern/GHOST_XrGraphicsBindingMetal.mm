/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GPU_framebuffer.hh"
#include "GPU_texture.hh"
#include "gpu_framebuffer_private.hh"

#include "mtl_context.hh"
#include "mtl_framebuffer.hh"
#include "mtl_texture.hh"

#include "GHOST_ContextMTL.hh"
#include "GHOST_XrException.hh"
#include "GHOST_XrGraphicsBindingMetal.h"

static MTLPixelFormat ghost_format_to_mtl_format(GHOST_TXrSwapchainFormat ghost_format,
                                                 bool expects_srgb_buffer)
{
  MTLPixelFormat format = MTLPixelFormatInvalid;

  switch (ghost_format) {
    case GHOST_kXrSwapchainFormatRGBA8:
      format = expects_srgb_buffer ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm;
      break;
    case GHOST_kXrSwapchainFormatRGBA16:
      format = MTLPixelFormatRGBA16Unorm;
      break;
    case GHOST_kXrSwapchainFormatRGBA16F:
      format = MTLPixelFormatRGBA16Float;
      break;
    case GHOST_kXrSwapchainFormatRGB10_A2:
      format = MTLPixelFormatRGB10A2Unorm;
      break;
  }

  if (format == MTLPixelFormatInvalid) {
    throw GHOST_XrException("No supported DirectX swapchain format found.");
  }

  return format;
}

static blender::gpu::TextureFormat ghost_format_to_gpu_format(GHOST_TXrSwapchainFormat ghost_format)
{
  using namespace blender::gpu;

  switch (ghost_format) {
    case GHOST_kXrSwapchainFormatRGBA8:
      /* TODO: Assume SRGB for now, storing the proper SRGB info in draw_info has bad side-effects.
       *       Will be fixed separately after dropping the use of the GPU backend. */
      return TextureFormat::SRGBA_8_8_8_8;
    case GHOST_kXrSwapchainFormatRGBA16:
      return TextureFormat::UNORM_16_16_16_16;
    case GHOST_kXrSwapchainFormatRGBA16F:
      return TextureFormat::SFLOAT_16_16_16_16;
    case GHOST_kXrSwapchainFormatRGB10_A2:
      return TextureFormat::UNORM_10_10_10_2;
  }
}

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

GHOST_XrGraphicsBindingMetal::GHOST_XrGraphicsBindingMetal(GHOST_Context &ghost_ctx)
{
  /* TODO: Pass the context to submitToSwapchainImage instead of storing a reference here. */
  ghost_metal_ctx_ = dynamic_cast<GHOST_ContextMTL *>(&ghost_ctx);
}

bool GHOST_XrGraphicsBindingMetal::checkVersionRequirements(GHOST_Context &ghost_ctx,
                                                            XrInstance instance,
                                                            XrSystemId system_id,
                                                            std::string *r_requirement_info) const
{
  XrGraphicsRequirementsMetalKHR gpu_requirements;
  gpu_requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR;
  gpu_requirements.next = nullptr;
  gpu_requirements.metalDevice = nullptr;

  PFN_xrGetMetalGraphicsRequirementsKHR xrGetMetalGraphicsRequirementsKHR_fn = nullptr;
  if (XR_FAILED(
          xrGetInstanceProcAddr(instance,
                                "xrGetMetalGraphicsRequirementsKHR",
                                (PFN_xrVoidFunction *)&xrGetMetalGraphicsRequirementsKHR_fn)))
  {
    return false;
  }

  xrGetMetalGraphicsRequirementsKHR_fn(instance, system_id, &gpu_requirements);

  GHOST_ContextMTL &ghost_metal_ctx = dynamic_cast<GHOST_ContextMTL &>(ghost_ctx);

  const bool metal_devices_match = (ghost_metal_ctx.metalDevice() == gpu_requirements.metalDevice);

  if (!metal_devices_match) {
    *r_requirement_info = "Metal Render Device used by Blender and OpenXR do not match";
    return false;
  }

  return true;
}

void GHOST_XrGraphicsBindingMetal::initFromGhostContext(GHOST_Context &ghost_ctx,
                                                        XrInstance /*instance*/,
                                                        XrSystemId /*system_id*/)
{
  GHOST_ContextMTL &ghost_metal_ctx = dynamic_cast<GHOST_ContextMTL &>(ghost_ctx);

  oxr_binding.metal.type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR;
  oxr_binding.metal.next = nullptr;
  oxr_binding.metal.commandQueue = (__bridge void *)ghost_metal_ctx.metalCommandQueue();
}

std::optional<int64_t> GHOST_XrGraphicsBindingMetal::chooseSwapchainFormat(
    const std::vector<int64_t> &runtime_formats,
    GHOST_TXrSwapchainFormat &r_format,
    bool &r_is_srgb_format) const
{
  std::vector<int64_t> gpu_binding_formats = {
      MTLPixelFormatRGBA16Float,
      MTLPixelFormatRGB10A2Unorm,
      MTLPixelFormatRGBA16Unorm,
      MTLPixelFormatRGBA8Unorm,
      MTLPixelFormatRGBA8Unorm_sRGB,
  };

  const std::optional result = choose_swapchain_format_from_candidates(gpu_binding_formats,
                                                                       runtime_formats);

  if (!result) {
    r_format = GHOST_kXrSwapchainFormatRGBA8;
    r_is_srgb_format = false;
    return result;
  }

  switch (*result) {
    case MTLPixelFormatRGB10A2Unorm:
      r_format = GHOST_kXrSwapchainFormatRGB10_A2;
      break;
    case MTLPixelFormatRGBA16Unorm:
      r_format = GHOST_kXrSwapchainFormatRGBA16;
      break;
    case MTLPixelFormatRGBA16Float:
      r_format = GHOST_kXrSwapchainFormatRGBA16F;
      break;
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
      r_format = GHOST_kXrSwapchainFormatRGBA8;
      break;
  }

  // TODO: Setting the proper srgb format value here causes side-effects for now, fix separately.
  // r_is_srgb_format = (*result == MTLPixelFormatRGBA8Unorm_sRGB);
  r_is_srgb_format = false;

  return result;
}

std::vector<XrSwapchainImageBaseHeader *> GHOST_XrGraphicsBindingMetal::createSwapchainImages(
    uint32_t image_count)
{
  std::vector<XrSwapchainImageMetalKHR> metal_images(image_count);
  std::vector<XrSwapchainImageBaseHeader *> base_images;

  /* Need to return vector of base header pointers, so of a different type. Need to build a new
   * list with this type, and keep the initial one alive. */
  for (XrSwapchainImageMetalKHR &image : metal_images) {
    image.type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
    base_images.push_back(reinterpret_cast<XrSwapchainImageBaseHeader *>(&image));
  }

  /* Keep alive. */
  image_cache_.push_back(std::move(metal_images));

  return base_images;
}

void GHOST_XrGraphicsBindingMetal::submitToSwapchainBegin() {}

void GHOST_XrGraphicsBindingMetal::submitToSwapchainImage(
    XrSwapchainImageBaseHeader &swapchain_image, const GHOST_XrDrawViewInfo &draw_info)
{
  // TODO: Potentially rewrite using the barebone Obj-C GHOST Metal Backend
  XrSwapchainImageMetalKHR &metal_swapchain_image = reinterpret_cast<XrSwapchainImageMetalKHR &>(
      swapchain_image);

  id<MTLTexture> metal_xr_texture = static_cast<id<MTLTexture>>(metal_swapchain_image.texture);

  using namespace blender;

  const gpu::TextureFormat tex_format = ghost_format_to_gpu_format(draw_info.swapchain_format);
  gpu::MTLTexture metal_gpu_texture = gpu::MTLTexture(
      "xr_swapchain_tex", tex_format, gpu::GPU_TEXTURE_2D, metal_xr_texture);

  gpu::MTLContext *ctx = gpu::MTLContext::get();

  gpu::MTLFrameBuffer *source_framebuffer = ctx->get_current_framebuffer();
  gpu::MTLFrameBuffer target_framebuffer = gpu::MTLFrameBuffer(ctx, "xr_target_fb");

  const int source_slot = 0;
  const int target_slot = 0;

  target_framebuffer.add_color_attachment(&metal_gpu_texture, target_slot, 0, 0);
  source_framebuffer->blit_to(GPU_COLOR_BIT,
                              source_slot,
                              static_cast<gpu::FrameBuffer *>(&target_framebuffer),
                              target_slot,
                              draw_info.ofsx,
                              draw_info.ofsy);
}

void GHOST_XrGraphicsBindingMetal::submitToSwapchainEnd() {}

bool GHOST_XrGraphicsBindingMetal::needsUpsideDownDrawing(GHOST_Context &ghost_ctx) const
{
  GHOST_ContextMTL &ghost_metal_ctx = dynamic_cast<GHOST_ContextMTL &>(ghost_ctx);

  return !ghost_metal_ctx.isUpsideDown();
}
