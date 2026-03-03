/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>
#include <utility>

#include "BLI_assert.h"
#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "GPU_shader.hh"

#include "COM_context.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

#include "COM_algorithm_morphological_distance_jump_flooding.hh"

namespace blender::compositor {

static void jump_flooding_pass_gpu(Context &context,
                                   const Result &input,
                                   const Result &input_jump_flooding_table,
                                   Result &output_jump_flooding_table,
                                   Result &output,
                                   const int step_size,
                                   const int radius,
                                   const MorphologicalOperatorType operator_type,
                                   const MorphologicalDistanceMetric /* distance_metric */,
                                   const bool is_initial_pass)
{
  gpu::Shader *shader = context.get_shader("compositor_morphological_distance_jump_flooding",
                                           ResultPrecision::Half);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1b(shader, "is_dilate", operator_type == MorphologicalOperatorType::Dilate);
  GPU_shader_uniform_1b(shader, "is_initial_pass", is_initial_pass);
  GPU_shader_uniform_1i(shader, "radius", radius);
  GPU_shader_uniform_1i(shader, "step_size", step_size);

  input.bind_as_texture(shader, "input_tx");
  input_jump_flooding_table.bind_as_texture(shader, "input_jump_flooding_table_tx");

  output_jump_flooding_table.bind_as_image(shader, "output_jump_flooding_table_img");
  output.bind_as_image(shader, "output_img");

  compute_dispatch_threads_at_least(shader, input.domain().data_size);

  GPU_shader_unbind();
  input.unbind_as_texture();
  input_jump_flooding_table.unbind_as_texture();
  output_jump_flooding_table.unbind_as_image();
  output.unbind_as_image();
}

/* TODO. */
static void jump_flooding_pass_cpu(const Result &input,
                                   const Result &input_jump_flooding_table,
                                   Result &output_jump_flooding_table,
                                   Result &output,
                                   const int step_size,
                                   const int radius,
                                   const MorphologicalOperatorType operator_type,
                                   const MorphologicalDistanceMetric /* distance_metric */,
                                   const bool is_initial_pass)
{
  const bool is_last_pass = step_size == 1;
  const bool is_dilate = operator_type == MorphologicalOperatorType::Dilate;
  const int squared_radius = radius * radius;
  const int2 size = input.domain().data_size;
  parallel_for(size, [&](const int2 texel) {
    /* For each of the previously flooded pixels in the 3x3 window of the given step size around
     * the center pixel, find the position of the closest seed pixel that is closest to the current
     * center pixel. */
    int2 chosen_texel = int2(0);
    int minimum_squared_distance = std::numeric_limits<int>::max();
    float chosen_value = is_dilate ? std::numeric_limits<float>::lowest() :
                                     std::numeric_limits<float>::max();

    for (int j = -1; j <= 1; j++) {
      for (int i = -1; i <= 1; i++) {
        const int2 neighbor_texel = texel + int2(i, j) * step_size;
        if (neighbor_texel.x < 0 || neighbor_texel.y < 0 || neighbor_texel.x >= size.x ||
            neighbor_texel.y >= size.y)
        {
          continue;
        }

        /* The flooding value is the texel of the closest seed pixel to this neighboring pixel. */
        const int2 neighbor_chosen_texel = is_initial_pass ?
                                               neighbor_texel :
                                               input_jump_flooding_table.load_pixel<int2>(
                                                   neighbor_texel);

        const float value = input.load_pixel<float>(neighbor_chosen_texel);
        if (is_dilate ? (value < chosen_value) : (value > chosen_value)) {
          continue;
        }

        /* Compute the squared distance to the neighbor's closest seed pixel. */
        const int squared_distance = math::distance_squared(neighbor_chosen_texel, texel);
        if (squared_distance > squared_radius) {
          continue;
        }

        if (value != chosen_value || squared_distance < minimum_squared_distance) {
          chosen_value = value;
          chosen_texel = neighbor_chosen_texel;
          minimum_squared_distance = squared_distance;
        }
      }
    }

    output_jump_flooding_table.store_pixel(texel, chosen_texel);
    if (is_last_pass) {
      output.store_pixel(texel, chosen_value);
    }
  });
}

static void jump_flooding_pass(Context &context,
                               const Result &input,
                               const Result &input_jump_flooding_table,
                               Result &output_jump_flooding_table,
                               Result &output,
                               const int step_size,
                               const int radius,
                               const MorphologicalOperatorType operator_type,
                               const MorphologicalDistanceMetric distance_metric,
                               const bool is_initial_pass)
{
  if (context.use_gpu()) {
    jump_flooding_pass_gpu(context,
                           input,
                           input_jump_flooding_table,
                           output_jump_flooding_table,
                           output,
                           step_size,
                           radius,
                           operator_type,
                           distance_metric,
                           is_initial_pass);
  }
  else {
    jump_flooding_pass_cpu(input,
                           input_jump_flooding_table,
                           output_jump_flooding_table,
                           output,
                           step_size,
                           radius,
                           operator_type,
                           distance_metric,
                           is_initial_pass);
  }
}

void morphological_distance_jump_flooding(Context &context,
                                          const Result &input,
                                          Result &output,
                                          const int radius,
                                          const MorphologicalOperatorType operator_type,
                                          const MorphologicalDistanceMetric distance_metric)
{
  BLI_assert(input.type() == ResultType::Float);
  BLI_assert(output.type() == ResultType::Float);

  output.allocate_texture(input.domain());

  /* We compute the result using two ping-pong buffers, so create two intermediate results. */
  Result intermediate_result_1 = context.create_result(ResultType::Int2, ResultPrecision::Half);
  Result intermediate_result_2 = context.create_result(ResultType::Int2, ResultPrecision::Half);
  intermediate_result_1.allocate_texture(input.domain());
  intermediate_result_2.allocate_texture(input.domain());

  /* Notice that result_to_flood is not yet initialized, but the first pass of the algorithm skips
   * reading from it, so no need to initialize it. */
  Result *result_to_flood = &intermediate_result_1;
  Result *result_after_flooding = &intermediate_result_2;

  /* The algorithm starts with a step size that is half the size of the image. However, the
   * algorithm assumes a square image that is a power of two in width without loss of generality.
   * To generalize that, we use half the next power of two of the maximum dimension. */
  const int first_step_size = power_of_2_max_i(radius);

  /* Successively apply a jump flooding pass, halving the step size every time and swapping the
   * ping-pong buffers. */
  int step_size = first_step_size;
  while (step_size != 0) {
    jump_flooding_pass(context,
                       input,
                       *result_to_flood,
                       *result_after_flooding,
                       output,
                       step_size,
                       radius,
                       operator_type,
                       distance_metric,
                       step_size == first_step_size);
    std::swap(result_to_flood, result_after_flooding);
    step_size /= 2;
  }

  result_to_flood->release();
  result_after_flooding->release();
}

}  // namespace blender::compositor
