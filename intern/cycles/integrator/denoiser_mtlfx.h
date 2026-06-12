/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_METALFX

#  include "integrator/denoiser_gpu.h"

CCL_NAMESPACE_BEGIN

/* Implementation of denoising API which uses MTLFXTemporalDenoisedScaler. */
class MetalFXDenoiser : public DenoiserGPU {
public:
  MetalFXDenoiser(Device *denoiser_device, const DenoiseParams &params);
  ~MetalFXDenoiser();
  
  virtual bool denoise_buffer(const BufferParams &buffer_params,
                              const BufferParams &denoised_buffer_params,
                              RenderBuffers *render_buffers,
                              int num_samples,
                              bool allow_inplace_modification,
                              float2 pixel_jitter) override;
  
  static bool is_device_supported(const DeviceInfo &device);
  
private:
  void *denoiser_desc_ = nullptr;
  void *mtlfx_denoiser_ = nullptr;
  
  virtual bool denoise_create_if_needed(DenoiseContext &context) override;
  
  virtual bool denoise_configure_if_needed(DenoiseContext &context) override;
  
  virtual bool denoise_run(const DenoiseContext &context, const DenoisePass &pass) override;
};

CCL_NAMESPACE_END

#endif
