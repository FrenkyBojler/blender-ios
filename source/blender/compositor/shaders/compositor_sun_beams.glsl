/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_hash.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_math_base_lib.glsl"

/* Returns a random position along the path between the texel and the source, which is
 * essentially a random value in the [0, steps] range to perform a quasi-monte carlo sampling.
 * The random values are generated using a low discrepancy quasirandom sequence based on the
 * following article:
 *
 *   "The Unreasonable Effectiveness of Quasirandom Sequences." Extreme Learning, 2021.
 *   https://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences.
 *
 * If jitter is not enabled, returns the i value instead. */
float get_position(int2 texel, int i, int steps)
{
#if defined(JITTER)
  double golden_ratio = 1.6180339887498948482;
  return float(fract(hash_uint2_to_float(texel.x, texel.y) + 1.0 / golden_ratio * i) * steps);
#else
  return i;
#endif
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 input_size = float2(texture_size(input_tx));

  /* The number of steps is the distance in pixels from the source to the current texel. With at
   * least a single step and at most the user specified maximum ray length, which is proportional
   * to the diagonal pixel count. */
  float unbounded_steps = max(1.0f, distance(float2(texel), source * input_size));
  int steps = min(max_steps, int(unbounded_steps));

  /* We integrate from the current pixel to the source pixel, so compute the start coordinates and
   * step vector in the direction to source. Notice that the step vector is still computed from the
   * unbounded steps, such that the total integration length becomes limited by the bounded steps,
   * and thus by the maximum ray length. */
  float2 coordinates = (float2(texel) + float2(0.5f)) / input_size;
  float2 vector_to_source = source - coordinates;
  float2 step_vector = vector_to_source / unbounded_steps;

  float accumulated_weight = 0.0f;
  float4 accumulated_color = float4(0.0f);

#if defined(JITTER)
  int number_of_steps = int(jitter_steps_ratio * steps);
#else
  int number_of_steps = steps;
#endif

  for (int i = 0; i <= number_of_steps; i++) {
    float position_index = get_position(texel, i, steps);
    float2 position = coordinates + position_index * step_vector;

    /* We are already past the image boundaries, if the jetter was activated then we have to
     * continue since we are sampling at random positions, on the  other hand if jetter wasn't
     * activated then any further steps are past the image so we break. */
    if (any(lessThan(position, float2(0.0f))) || any(greaterThan(position, float2(1.0f)))) {
#if defined(JITTER)
      continue;
#endif
      break;
    }

    float4 sample_color = texture(input_tx, position);

    /* Attenuate the contributions of pixels that are further away from the source using a
     * quadratic falloff. */
    float weight = square(1.0f - position_index / float(steps));

    accumulated_weight += weight;
    accumulated_color += sample_color * weight;
  }

  accumulated_color /= accumulated_weight != 0.0f ? accumulated_weight : 1.0f;
  imageStore(output_img, texel, accumulated_color);
}
