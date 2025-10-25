/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_sample_rect.glsl"

void main()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);

  float2 uv = to_float2x2(inverse_matrix) * float2(texel) + inverse_matrix[2].xy;

  // derivative was calculated by caller and is in wh. Equivalent code:
  // float2 wh = hypot(inverse_matrix[0].xy, inverse_matrix[1].xy);

  imageStore(domain_img, texel, sample_rect(uv, wh));
}
