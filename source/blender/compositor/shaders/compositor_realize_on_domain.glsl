/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_realize_on_domain_infos.hh"

#include "gpu_shader_compositor_sample_rect.glsl"

void realize_on_domain_box()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 uv = to_float2x2(inverse_matrix) * float2(texel) + inverse_matrix[2].xy;
  imageStore(domain_img, texel, sample_box(uv, wh));
}

void realize_on_domain_bspline()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 uv = to_float2x2(inverse_matrix) * float2(texel) + inverse_matrix[2].xy;
  imageStore(domain_img, texel, sample_bspline(uv, wh));
}

/* For Nearest & Bilinear sampline, matrix has been pre-multiplied to produce
 * uv values in the range 0-1, and wh is not needed.
 */
void realize_on_domain_texture()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 P = to_float2x2(inverse_matrix) * float2(texel) + inverse_matrix[2].xy;
  imageStore(domain_img, texel, texture(input_tx, P, 0));
}

void realize_on_domain_anisotropic()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 P = to_float2x2(inverse_matrix) * float2(texel) + inverse_matrix[2].xy;
  float2 dPdx = inverse_matrix[0].xy;
  float2 dPdy = inverse_matrix[1].xy;
  imageStore(domain_img, texel, textureGrad(input_tx, P, dPdx, dPdy));
}
