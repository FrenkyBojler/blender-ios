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

/* Samples a pixel from a GPU texture. */
template<typename T> T sample_pixel(Context &context, const Result &result, const int2 texel)
{
  BLI_assert((texel.x >= 0) && (texel.y >= 0) && (texel.x < result.domain().size.x) &&
             (texel.y < result.domain().size.y));

  GPUShader *shader = context.get_shader("compositor_sample_pixel");
  GPU_shader_bind(shader);

  GPU_shader_uniform_2iv(shader, "texel", texel);

  GPUTexture *texture_1x1 = gpu::TexturePool::get().acquire_texture(
      1,
      1,
      Result::gpu_texture_format(ResultType::Color, ResultPrecision::Full),
      GPU_TEXTURE_USAGE_GENERAL);

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  const int input_image = GPU_shader_get_sampler_binding(shader, "input_tx");
  GPU_texture_bind(result, input_image);

  const int output_image = GPU_shader_get_sampler_binding(shader, "output_img");
  GPU_texture_image_bind(texture_1x1, output_image);

  GPU_compute_dispatch(shader, 1, 1, 1);

  GPU_texture_image_unbind(texture_1x1);
  GPU_texture_unbind(result);

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  float *pixel = static_cast<float *>(GPU_texture_read(texture_1x1, GPU_DATA_FLOAT, 0));
  gpu::TexturePool::get().release_texture(texture_1x1);

  T sampled_value = T(pixel);
  MEM_freeN(pixel);
  GPU_shader_unbind();

  return sampled_value;
}

template float4 sample_pixel<float4>(Context &context,
                                           const Result &result,
                                           const int2 texel);

}  // namespace blender::compositor
