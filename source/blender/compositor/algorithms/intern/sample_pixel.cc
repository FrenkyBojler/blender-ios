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
                        const float2 coordinates)
{
  GPUShader *shader = context.get_shader(get_pixel_sampler_shader_name(interpolation));
  GPU_shader_bind(shader);

  GPU_shader_uniform_2fv(shader, "coordinates", coordinates);

  Result output = context.create_result(input_texture.type(), ResultPrecision::Full);
  output.allocate_texture(int2(1));

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

  input_texture.bind_as_texture(shader, "input_tx");
  output.bind_as_image(shader, "output_img");

  GPU_compute_dispatch(shader, 1, 1, 1);

  input_texture.unbind_as_texture();
  output.unbind_as_image();
  GPU_shader_unbind();

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  float *pixel = static_cast<float *>(GPU_texture_read(output, GPU_DATA_FLOAT, 0));
  output.release();

  float4 sampled_value = float4(0.0f, 0.0f, 0.0f, 1.0f);
  output.get_cpp_type().copy_assign(pixel, sampled_value);

  MEM_freeN(pixel);

  return sampled_value;
}

/* Samples a pixel from a CPU texture. */
float4 sample_pixel_cpu(const Result &input_texture,
                        const Interpolation &interpolation,
                        const ExtensionMode &extension_mode_x,
                        const ExtensionMode &extension_mode_y,
                        const float2 coordinates)
{
  return input_texture.sample(coordinates, interpolation, extension_mode_x, extension_mode_y);
}

/* Samples a pixel from a texture. */
float4 sample_pixel(Context &context,
                    const Result &input_texture,
                    const Interpolation &interpolation,
                    const ExtensionMode &extension_mode_x,
                    const ExtensionMode &extension_mode_y,
                    const float2 coordinates)
{
  if (input_texture.is_single_value()) {
    switch (input_texture.type()) {
      case ResultType::Float:
        return float4(input_texture.get_single_value<float>(), 0.0f, 0.0f, 1.0f);
      case ResultType::Float2:
        return float4(input_texture.get_single_value<float2>(), 0.0f, 1.0f);
      case ResultType::Float3:
        return float4(input_texture.get_single_value<float3>(), 1.0f);
      case ResultType::Float4:
      case ResultType::Color:
        return input_texture.get_single_value<float4>();
      default:
        break;
    }

    BLI_assert_unreachable();
    return float4(0.0f);
  }
  if (context.use_gpu()) {
    return sample_pixel_gpu(
        context, input_texture, interpolation, extension_mode_x, extension_mode_y, coordinates);
  }
  else {
    return sample_pixel_cpu(
        input_texture, interpolation, extension_mode_x, extension_mode_y, coordinates);
  }
}

}  // namespace blender::compositor
