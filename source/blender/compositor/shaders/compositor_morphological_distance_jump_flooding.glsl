/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* TODO. */

#include "infos/compositor_morphological_distance_jump_flooding_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_morphological_distance_jump_flooding)

#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  const bool is_last_pass = step_size == 1;
  const int2 size = texture_size(input_tx);
  const float squared_radius = radius * radius;

  /* For each of the previously flooded pixels in the 3x3 window of the given step size around the
   * center pixel, find the position of the closest seed pixel that is closest to the current
   * center pixel. */
  int2 chosen_texel = int2(0.0f);
  float minimum_squared_distance = FLT_MAX;
  float chosen_value = is_dilate ? -FLT_MAX : FLT_MAX;
  for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
      const int2 neighbor_texel = texel + int2(i, j) * step_size;
      if (neighbor_texel.x < 0 || neighbor_texel.y < 0 || neighbor_texel.x >= size.x ||
          neighbor_texel.y >= size.y)
      {
        continue;
      }

      int2 neighbor_chosen_texel =
          is_initial_pass ? neighbor_texel :
                            texture_load_unbound(input_jump_flooding_table_tx, neighbor_texel).xy;

      const float value = texture_load_unbound(input_tx, neighbor_chosen_texel).x;
      if (is_dilate ? (value < chosen_value) : (value > chosen_value)) {
        continue;
      }

      /* Compute the squared distance to the neighbor's closest seed pixel. */
      const float squared_distance = distance_squared(float2(neighbor_chosen_texel),
                                                      float2(texel));
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

  imageStore(output_jump_flooding_table_img, texel, int4(chosen_texel, int2(0)));
  if (is_last_pass) {
    imageStore(output_img, texel, float4(chosen_value));
  }
}
