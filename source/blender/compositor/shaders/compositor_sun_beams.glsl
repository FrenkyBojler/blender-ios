/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_hash.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_math_base_lib.glsl"

/* 2D hash (iqint3) recommended from "Hash Functions for GPU Rendering" JCGT Vol. 9, No. 3, 2020
 * https://jcgt.org/published/0009/03/02/ */
float hash_iqint3_f(uint2 x)
{
  uint2 q = 1103515245u * ((x >> 1u) ^ (x.yx));
  uint n = 1103515245u * ((q.x) ^ (q.y >> 3u));
  return float(n) * (1.0 / float(0xffffffffu));
}

/* Generates a low-discrepancy quasirandom value in the [0, 1) range using the R1 sequence.
 *
 * This implementation is based on the quasirandom sequence described in:
 *
 *   "The Unreasonable Effectiveness of Quasirandom Sequences." Extreme Learning, 2021.
 *   https://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences
 *
 * Additionally, it incorporates the enhancement proposed in:
 *
 *   "A Better R2 Sequence." Marty's Mods, 2022.
 *   https://www.martysmods.com/a-better-r2-sequence
 *
 * The sequence uses a toroidal combined with a scaled irrational increment
 * derived from the golden ratio to ensure well-distributed, non-repeating samples.
 * The improved formulation significantly extends usable index range under floating-point
 * precision constraints while preserving the low-discrepancy property.
 */
float r1_low_discrepancy_sequence(const float seed, const int i)
{
  constexpr float golden_ratio = 1.618033988749894848204586834365638118;
  return fract(-seed + (1.0f - 1.0f / golden_ratio) * i);
}

/* Returns an index for a position along the path between the texel and the source.
 *
 * If jitter is enabled, the position index is determined using a low-discrepancy
 * quasirandom sequence to perform quasi-Monte Carlo sampling over the range [0, steps].
 * Otherwise, it returns the integer index `i` directly.
 */
float get_sample_position(int2 texel, int i, int steps)
{
#if defined(JITTER)
  float seed = hash_iqint3_f(texel);
  return r1_low_discrepancy_sequence(seed, i) * steps;
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

  float accumulated_weight = 1.0f;
  float4 accumulated_color = texture(input_tx, coordinates);

#if defined(JITTER)
  int number_of_steps = int((1.0f - jitter_factor) * steps);
#else
  int number_of_steps = steps;
#endif

  for (int i = 0; i <= number_of_steps; i++) {
    float position_index = get_sample_position(texel, i, steps);
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
