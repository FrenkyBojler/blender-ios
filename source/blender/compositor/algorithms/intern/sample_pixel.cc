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
    case Interpolation::Bicubic:
      return "compositor_sample_pixel_bicubic";
    case Interpolation::Bilinear:
    case Interpolation::Nearest:
      return "compositor_sample_pixel";
  }
  BLI_assert_unreachable();
  return "compositor_sample_pixel";
}

/* Samples a pixel from a GPU texture. */
float4 sample_pixel_gpu(Context &context,
                        const Result &input_texture,
                        const Interpolation &interpolation,
                        const ExtensionMode &extension_mode_x,
                        const ExtensionMode &extension_mode_y,
                        const float2 uv_coordinates)
{
  GPUShader *shader = context.get_shader(get_pixel_sampler_shader_name(interpolation));
  GPU_shader_bind(shader);

  GPU_shader_uniform_2fv(shader, "uv_coordinates", uv_coordinates);

  Result texture_1x1 = context.create_result(input_texture.type(), ResultPrecision::Full);
  texture_1x1.allocate_texture(int2(1));

  if (interpolation == Interpolation::Anisotropic) {
    GPU_texture_anisotropic_filter(input_texture, true);
    GPU_texture_mipmap_mode(input_texture, true, true);
  }
  else {
    const bool use_bilinear = ELEM(interpolation, Interpolation::Bilinear, Interpolation::Bicubic);
    GPU_texture_filter_mode(input_texture, use_bilinear);
  }

  GPU_texture_extend_mode_x(input_texture, map_extension_mode_to_extend_mode(extension_mode_x));
  GPU_texture_extend_mode_y(input_texture, map_extension_mode_to_extend_mode(extension_mode_y));

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);

  input_texture.bind_as_texture(shader, "input_tx");

  const int output_image = GPU_shader_get_sampler_binding(shader, "output_img");
  GPU_texture_image_bind(texture_1x1, output_image);

  GPU_compute_dispatch(shader, 1, 1, 1);

  GPU_texture_image_unbind(texture_1x1);
  input_texture.unbind_as_texture();

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  float *pixel = static_cast<float *>(GPU_texture_read(texture_1x1, GPU_DATA_FLOAT, 0));
  gpu::TexturePool::get().release_texture(texture_1x1);

  float4 sampled_value;
  for (int i = 0; i < input_texture.channels_count(); i++) {
    sampled_value[i] = pixel[i];
  }
  MEM_freeN(pixel);
  GPU_shader_unbind();

  return sampled_value;
}

/* Samples a pixel from a CPU texture. */
float4 sample_pixel_cpu(const Result &input_texture,
                        const Interpolation &interpolation,
                        const ExtensionMode &extension_mode_x,
                        const ExtensionMode &extension_mode_y,
                        const float2 uv_coordinates)
{
  return input_texture.sample(uv_coordinates, interpolation, extension_mode_x, extension_mode_y);
}

/* Samples a pixel from a texture. */
float4 sample_pixel(Context &context,
                    const Result &input_texture,
                    const Interpolation &interpolation,
                    const ExtensionMode &extension_mode_x,
                    const ExtensionMode &extension_mode_y,
                    const float2 uv_coordinates)
{
  if (input_texture.is_single_value()) {
    switch (input_texture.type()) {
      case ResultType::Float:
        return float4(input_texture.get_single_value<float>(), 0.0, 0.0, 0.0);
      case ResultType::Float2:
        return float4(input_texture.get_single_value<float2>(), 0.0, 0.0);
      case ResultType::Float3:
        return float4(input_texture.get_single_value<float3>(), 0.0);
      case ResultType::Float4:
      case ResultType::Color:
        return input_texture.get_single_value<float4>();
    }

    BLI_assert_unreachable();
    return float4(0.0);
  }
  if (context.use_gpu()) {
    return sample_pixel_gpu(
        context, input_texture, interpolation, extension_mode_x, extension_mode_y, uv_coordinates);
  }
  else {
    return sample_pixel_cpu(
        input_texture, interpolation, extension_mode_x, extension_mode_y, uv_coordinates);
  }
}

}  // namespace blender::compositor
