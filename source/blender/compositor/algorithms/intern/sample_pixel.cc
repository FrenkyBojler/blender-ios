/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "MEM_guardedalloc.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_texture_pool.hh"

#include "COM_context.hh"
#include "COM_result.hh"

#include "COM_algorithm_sample_pixel.hh"

namespace blender::compositor {

char const *get_pixel_sampler_shader_name(const Interpolation &interpolation)
{
  switch (interpolation) {
    case Interpolation::Anisotropic:
      return "compositor_map_uv_anisotropic";
    case Interpolation::Bicubic:
      return "compositor_map_uv_bicubic";
    case Interpolation::Bilinear:
    case Interpolation::Nearest:
      return "compositor_map_uv";
  }
  BLI_assert_unreachable();
  return "compositor_map_uv";
}

/* Samples a pixel from a GPU texture. */
float4 sample_pixel_gpu(Context &context,
                        const Result &input_image,
                        const Interpolation &interpolation,
                        const float2 uv_coordinates)
{
  GPUShader *shader = context.get_shader(get_pixel_sampler_shader_name(interpolation));
  GPU_shader_bind(shader);

  GPU_shader_uniform_1b(shader, "is_single_value_uv_coordinates", true);
  GPU_shader_uniform_2fv(shader, "single_value_uv_coordinates", uv_coordinates);

  GPUTexture *texture_1x1 = gpu::TexturePool::get().acquire_texture(
      1,
      1,
      Result::gpu_texture_format(ResultType::Color, ResultPrecision::Full),
      GPU_TEXTURE_USAGE_GENERAL);

  if (interpolation == Interpolation::Anisotropic) {
    GPU_texture_anisotropic_filter(input_image, true);
    GPU_texture_mipmap_mode(input_image, true, true);
  }
  else {
    const bool use_bilinear = ELEM(interpolation, Interpolation::Bilinear, Interpolation::Bicubic);
    GPU_texture_filter_mode(input_image, use_bilinear);
  }

  GPU_texture_extend_mode(input_image, GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER);

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);

  input_image.bind_as_texture(shader, "input_tx");

  const int output_image = GPU_shader_get_sampler_binding(shader, "output_img");
  GPU_texture_image_bind(texture_1x1, output_image);

  GPU_compute_dispatch(shader, 1, 1, 1);

  GPU_texture_image_unbind(texture_1x1);
  input_image.unbind_as_texture();

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  float *pixel = static_cast<float *>(GPU_texture_read(texture_1x1, GPU_DATA_FLOAT, 0));
  gpu::TexturePool::get().release_texture(texture_1x1);

  float4 sampled_value = float4(pixel);
  MEM_freeN(pixel);
  GPU_shader_unbind();

  return sampled_value;
}

/* Samples a pixel from a CPU texture. */
float4 sample_pixel_cpu(const Result &input_image,
                        const Interpolation &interpolation,
                        const float2 uv_coordinates)
{
  switch (interpolation) {
    case Interpolation::Nearest:
      return input_image.sample_nearest_zero(uv_coordinates);
      break;
    case Interpolation::Bilinear:
      return input_image.sample_bilinear_zero(uv_coordinates);
      break;
    /* NOTE: The anisotropic case should be handled after reimplementation of EWA. */
    case Interpolation::Anisotropic:
    case Interpolation::Bicubic:
      return input_image.sample_cubic_wrap(uv_coordinates, false, false);
      break;
  }
  BLI_assert_unreachable();
  return float4(0.0, 0.0, 0.0, 0.0);
}

/* Samples a pixel from a texture. */
float4 sample_pixel(Context &context,
                    const Result &input_image,
                    const Interpolation &interpolation,
                    const float2 uv_coordinates)
{
  if (context.use_gpu()) {
    return sample_pixel_gpu(context, input_image, interpolation, uv_coordinates);
  }
  else {
    return sample_pixel_cpu(input_image, interpolation, uv_coordinates);
  }
}

}  // namespace blender::compositor
