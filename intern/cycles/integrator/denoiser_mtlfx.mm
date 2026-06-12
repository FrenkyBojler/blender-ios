/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_METALFX

#  include "integrator/denoiser_mtlfx.h"
#  include "integrator/pass_accessor_gpu.h"

#  include "device/metal/device_impl.h"
#  include "device/metal/queue.h"

#  include <MetalFX/MetalFX.h>
#  include <MetalFX/MTLFXTemporalDenoisedScaler.h>

CCL_NAMESPACE_BEGIN

MetalFXDenoiser::MetalFXDenoiser(Device *denoiser_device, const DenoiseParams &params)
  : DenoiserGPU(denoiser_device, params)
{
  
}

MetalFXDenoiser::~MetalFXDenoiser()
{
  
}

bool MetalFXDenoiser::denoise_buffer(const BufferParams &buffer_params,
                                     const BufferParams &denoised_buffer_params,
                                     RenderBuffers *render_buffers,
                                     int num_samples,
                                     bool allow_inplace_modification,
                                     float2 pixel_jitter)
{
  return true;
}

bool MetalFXDenoiser::is_device_supported(const DeviceInfo &device)
{
  if (device.type == DEVICE_METAL) {
    if (@available(macos 26.0, *)) {
      return device.denoisers & DENOISER_MTLFX;
    }
  }
  return false;
}

bool MetalFXDenoiser::denoise_create_if_needed(DenoiseContext &context)
{
  if (mtlfx_denoiser_ != nullptr) {
    return false;
  }

  if (@available(macos 26.0, *)) {
    MTLFXTemporalDenoisedScalerDescriptor *desc = [[MTLFXTemporalDenoisedScalerDescriptor alloc] init];
    BufferParams buffer_params = context.buffer_params;
    
    desc.autoExposureEnabled = false;
    desc.reactiveMaskTextureEnabled = false;
    desc.transparencyOverlayTextureEnabled = false;
    desc.denoiseStrengthMaskTextureEnabled = false;
    desc.specularHitDistanceTextureEnabled = false;
    desc.inputWidth = buffer_params.width;
    desc.inputHeight = buffer_params.height;
    desc.outputWidth = buffer_params.width;
    desc.outputHeight = buffer_params.height;

    desc.colorTextureFormat = MTLPixelFormatRGBA16Float;
    desc.depthTextureFormat = MTLPixelFormatDepth32Float;
    desc.motionTextureFormat = MTLPixelFormatRG16Float;
    
    desc.normalTextureFormat = MTLPixelFormatRGBA16Float;
    desc.roughnessTextureFormat = MTLPixelFormatR16Float;
    desc.diffuseAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
    desc.specularAlbedoTextureFormat = MTLPixelFormatRGBA16Float;

    desc.outputTextureFormat = MTLPixelFormatRGBA16Float;
    mtlfx_denoiser_ = (id<MTLFXTemporalDenoisedScaler>)[desc newTemporalDenoisedScalerWithDevice:MTLCreateSystemDefaultDevice()];
    
    return true;
  }
  
  return false;
}

bool MetalFXDenoiser::denoise_configure_if_needed(DenoiseContext &context)
{
  return true;
}

bool MetalFXDenoiser::denoise_run(const DenoiseContext &context, const DenoisePass &pass)
{
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>) denoiser_queue_->native_queue();
  id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
  
  printf("mtlfx_denoiser_ = %p\n", mtlfx_denoiser_);
  
  [(id<MTLFXTemporalDenoisedScaler>) mtlfx_denoiser_ encodeToCommandBuffer:command_buffer];
    
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  
  return true;
}

CCL_NAMESPACE_END

#endif
