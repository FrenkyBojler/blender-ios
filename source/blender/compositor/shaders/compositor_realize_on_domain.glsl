/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_sampleRect.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  // Calculate derivatives of coordinates
  float2 dPdx = inverse_transformation[0].xy;
  float2 dPdy = inverse_transformation[1].xy;
  // convert to rectangle (todo: this is constant)
  float2 wh = hypot2(dPdx, dPdy);

  /* Transform the input image by transforming the domain coordinates with the inverse of input
   * image's transformation. The inverse transformation is an affine matrix and thus the
   * coordinates should be in homogeneous coordinates.
   * Include adjustment because matrix is between pixel corners, not pixel centers.
  */
  float2 uv = (to_float3x3(inverse_transformation) * float3(float2(texel) + 0.5f, 1.0f)).xy - 0.5f;

  imageStore(domain_img, texel, sampleRect(uv, wh));
}
