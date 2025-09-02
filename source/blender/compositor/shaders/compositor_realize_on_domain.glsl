/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_sample_rect.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  // float2 uv = (to_float3x3(imat) * float3(texel.x, texel.y, 1.0f)).xy;
  float2 uv = (imat[0].xy * texel.x) + (imat[1].xy * texel.y) + imat[2].xy;

  // derivative was calculated by caller and is in wh. Equivalent code:
  // float2 wh = hypot2(imat[0].xy, imat[1].xy);

  imageStore(domain_img, texel, sample_rect(uv, wh));
}
