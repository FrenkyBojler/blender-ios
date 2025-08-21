/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_sampleRect.glsl"

void main()
{
  // derivative was calculated by caller and is in wh. Equivalent code:
  // float2 wh = hypot2(imat[0].xy, imat[1].xy);

  int2 texel = int2(gl_GlobalInvocationID.xy);
  // transform to input texels
  float2 uv = (to_float3x3(imat) * float3(texel.x, texel.y, 1.0f)).xy;

  imageStore(domain_img, texel, sampleRect(uv, wh));
}
