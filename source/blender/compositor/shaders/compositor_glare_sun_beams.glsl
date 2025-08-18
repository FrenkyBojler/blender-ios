/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_hash.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_math_base_lib.glsl"

/* 2D hash (iqint3), originally recommended in
 * "Hash Functions for GPU Rendering", JCGT Vol. 9, No. 3, 2020:
 * https://jcgt.org/published/0009/03/02/
 *
 * Updated with modifications inspired by the "Star Nest" fragment shader
 * by Pablo Román Andrioli on ShaderToy:
 * https://www.shadertoy.com/view/4tXyWN
 */
float hash_iqint3_f(uint2 p)
{
  p *= uvec2(73333, 7777);
  p ^= (uvec2(3333777777) >> (p >> 28));
  uint n = p.x * p.y;
  return float(n ^ (n >> 15)) * (1.0f / float(0xffffffffU));
}

/* Generates a low-discrepancy quasirandom value in the [0, 1) range using
 * the Fibonacci Word Sampling method.
 *
 * This implementation is based on the sequence described in:
 *
 *   "Fibonacci Word Sampling: A Sorted Golden Ratio Low Discrepancy Sequence."
 *   Demofox Blog, 2023.
 *   https://blog.demofox.org/2023/02/17/fibonacci-word-sampling-a-sorted-golden-ratio-low-discrepancy-sequence/
 *
 * The method constructs sample positions by iteratively dividing the unit
 * interval into "big" and "small" gaps, following the structure of the
 * Fibonacci word. The relative gap sizes are derived from the golden ratio,
 * producing evenly spread points with only two distinct spacing values.
 */

  float FibonacciWordSequenceNext(float last, inout float run, int steps)
  {
    constexpr float c_goldenRatio = 1.61803398875f;
    constexpr float c_goldenRatioConjugate = 0.61803398875f;
    float big = 1.0f / float((1.0f - jitter_factor) * steps);
    float small = big * c_goldenRatioConjugate;
    run += c_goldenRatio;
    float shift = floor(run);
    run -= shift;
    return (last /steps + ((shift == 1.0f) ? small : big)) * steps;
  }

/* Returns an index for a position along the path between the texel and the source.
 *
 * If jitter is enabled, the position index is determined using a low-discrepancy
 * quasirandom sequence to perform quasi-Monte Carlo sampling over the range [0, steps].
 * Otherwise, it returns the integer index `i` directly.
 */
float get_sample_position(const int i, const int steps, inout float run, float position_index)
{
#if defined(JITTER)
  return FibonacciWordSequenceNext(position_index, run, steps);
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
  int number_of_steps = int((1.0f - jitter_factor) * steps);
#else
  int number_of_steps = steps;
#endif

  float seed = hash_iqint3_f(uint2(texel));
  float run = 0.61803398875f;
  float position_index = 0.0f;

  for (int i = 0; i <= number_of_steps; i++) {
    position_index = get_sample_position(i, steps, run, position_index);
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

  if (accumulated_weight != 0.0f) {
    accumulated_color /= accumulated_weight;
  }
  else {
    accumulated_color = texture(input_tx, coordinates);
  }
  imageStore(output_img, texel, accumulated_color);
}
