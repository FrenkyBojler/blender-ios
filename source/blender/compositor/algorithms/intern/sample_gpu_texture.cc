/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <limits>

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_task.hh"

#include "MEM_guardedalloc.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_texture_pool.hh"

#include "COM_context.hh"
#include "COM_result.hh"

#include "COM_algorithm_parallel_reduction.hh"

namespace blender::compositor {

/* Samples a pixel from a GPU texture. */
float sample_gpu_texture(Context &context, const Result &result, const int2 texel)
{
  BLI_assert((texel.x >= 0) && (texel.y >= 0) && (texel.x < result.domain().size.x) &&
             (texel.y < result.domain().size.y));

  GPUShader *shader = context.get_shader("sample_gpu_texture");
  GPU_shader_bind(shader);

  GPU_shader_uniform_2iv(shader, "texel", texel);

  GPUTexture *texture_1x1 = gpu::TexturePool::get().acquire_texture(
      1,
      1,
      Result::gpu_texture_format(ResultType::Float, ResultPrecision::Full),
      GPU_TEXTURE_USAGE_GENERAL);

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  const int input_image = GPU_shader_get_sampler_binding(shader, "input_tx");
  GPU_texture_bind(result, input_image);

  const int output_image = GPU_shader_get_sampler_binding(shader, "output_img");
  GPU_texture_image_bind(texture_1x1, output_image);

  GPU_compute_dispatch(shader, 1, 1, 1);

  GPU_texture_image_unbind(result);
  GPU_texture_unbind(texture_1x1);

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  float *pixel = static_cast<float *>(GPU_texture_read(texture_1x1, GPU_DATA_FLOAT, 0));
  gpu::TexturePool::get().release_texture(texture_1x1);

  float sampled_value = *pixel;
  MEM_freeN(pixel);
  GPU_shader_unbind();

  return sampled_value;
}

}  // namespace blender::compositor
