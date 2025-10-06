/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <complex>
#include <numeric>

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_fftw.hh"
#include "BLI_index_range.hh"
#include "BLI_memory_utils.hh"
#include "BLI_task.hh"

#include "COM_algorithm_distance_field.hh"
#include "COM_context.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"
#include "COM_algorithm_jump_flooding.hh"

namespace blender::compositor {

void distance_field(Context &context,
              const Result &input,
              Result &nearest,
              Result &distance,
              bool is_signed,
              bool normalize)
{
  Result edges = context.create_result(ResultType::Int2, ResultPrecision::Half);

  // find all of the edges of the matte
  calculate_edges(context, input, edges, false);

  // calculate the nearest edge to each pixel
  calculate_nearest(context, edges, nearest, normalize);

  // calculate the distance from the nearest edge to the pixel
  position_to_distance(context, input, nearest, distance, is_signed, normalize);

  edges.release();
}

void calculate_edges(Context &context,
              const Result &input,
              Result &edges,
              bool include_diagonal)
{
  if (context.use_gpu()) {
    calculate_edges_gpu(context, input, edges, include_diagonal);
  }
  else {
    calculate_edges_cpu(context, input, edges, include_diagonal);
  }
}

void calculate_edges_cpu(Context &context,
              const Result &input,
              Result &edges,
              bool include_diagonal)
{
  const Domain domain = input.domain();
  edges.allocate_texture(domain);

  parallel_for(domain.size, [&](const int2 texel) {
      /* Identify if any of the 8 neighbors around the center pixel are not masked. */
      
      bool is_masked = input.load_pixel_extended<float>(texel) != 0.0f;
      bool is_edge = false;

      if (is_masked)
      {
        bool has_masked_neighbors = false;
        bool has_non_masked_neighbors = false;
        for (int j = -1; j <= 1; j++) {
          for (int i = -1; i <= 1; i++) {
            int2 offset = int2(i, j);
  
            /* Exempt the center pixel. */
            if (offset == int2(0)) {
              continue;
            }

            if (!include_diagonal) {
              if (abs(j) == abs(i))
                continue;
            }
  
            if (input.load_pixel_extended<float>(texel + offset) == 0.0f) {
              has_non_masked_neighbors = true;
            }
            else {
              has_masked_neighbors = true;
            }
  
            /* Both are true, no need to continue. */
            if (has_non_masked_neighbors && has_masked_neighbors) {
              break;
            }
          }
        }

        is_edge = (has_non_masked_neighbors && has_masked_neighbors);
      }
      
      int2 jump_flood_value = initialize_jump_flooding_value(texel, is_edge);

      edges.store_pixel(texel, jump_flood_value);
    });
}              

void calculate_edges_gpu(Context &context,
              const Result &input,
              Result &edges,
              bool include_diagonal)
{
  gpu::Shader *shader = context.get_shader("compositor_distance_field_calculate_edges",
                                               ResultPrecision::Half);
  
  GPU_shader_bind(shader);

  GPU_shader_uniform_1b(shader, "include_diagonal", include_diagonal);   
  input.bind_as_texture(shader, "mask_tx");             
  
  const Domain domain = input.domain();
  edges.allocate_texture(domain);
  edges.bind_as_image(shader, "edges_img");


  compute_dispatch_threads_at_least(shader, domain.size);


  input.unbind_as_texture();
  edges.unbind_as_image();

  GPU_shader_unbind();
}

void calculate_nearest(Context &context,
              Result &edges,
              Result &output,
              bool normalize)
{
  jump_flooding(context, edges, output);
}

void position_to_distance(Context &context,
              const Result &matte,
              const Result &positions,
              Result &output,
              bool is_signed,
              bool normalize)
{
  if (context.use_gpu()) {
    position_to_distance_gpu(context, matte, positions, output, is_signed, normalize);
  }
  else {
    position_to_distance_cpu(context, matte, positions, output, is_signed, normalize);
  }
}

void position_to_distance_cpu(Context &context,
              const Result &matte,
              const Result &positions,
              Result &output,
              bool is_signed,
              bool normalize)
{
  const Domain domain = positions.domain();
  output.allocate_texture(domain);

  float imageMagnitude = math::max(domain.size.x, domain.size.y);

  parallel_for(domain.size, [&](const int2 texel) {
      /* Identify if any of the 8 neighbors around the center pixel are not masked. */
      
      int2 pos = positions.load_pixel_extended<int2>(texel);

      float distance = len_v2(float2(pos - texel));

      if (normalize)
        distance = distance / imageMagnitude;

      // Set the sign of the pixel
      if (is_signed && matte.load_pixel_extended<float>(texel) != 0.0f)
        distance *= -1;

      output.store_pixel(texel, distance);
    });
}

void position_to_distance_gpu(Context &context,
              const Result &matte,
              const Result &positions,
              Result &output,
              bool is_signed,
              bool normalize)
{
  gpu::Shader *shader = context.get_shader("compositor_distance_field_calculate_distance",
                                               ResultPrecision::Half);
  
  GPU_shader_bind(shader);

  GPU_shader_uniform_1b(shader, "should_normalize", normalize);
  GPU_shader_uniform_1b(shader, "is_signed", is_signed);   
  
  matte.bind_as_texture(shader, "mask_tx");
  positions.bind_as_texture(shader, "positions_tx");      
  
  const Domain domain = positions.domain();
  output.allocate_texture(domain);
  output.bind_as_image(shader, "dist_img");


  compute_dispatch_threads_at_least(shader, domain.size);


  matte.unbind_as_texture();
  positions.unbind_as_texture();
  output.unbind_as_image();

  GPU_shader_unbind();
}

}  // namespace blender::compositor
