/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_realize_on_domain_infos.hh"

#include "gpu_shader_math_sample_rect_lib.glsl"

template<enum Sampler sampler> void realize_on_domain()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const float2 uv = to_float2x2(transformation) * float2(texel) + transformation[2].xy;
  const float2 scale = float2(textureSize(input_tx, 0)); // temporary convert from normalized to texels
  imageStore(domain_img, texel, sample_rect<sampler>(input_tx, uv * scale, wh * scale));
}

template void realize_on_domain<Sampler::Box>;
template void realize_on_domain<Sampler::Bspline>;

/* For Nearest & Bilinear sampling, matrix has been pre-multiplied to produce
 * uv values in the range 0-1, and wh is not needed.
 */
void realize_on_domain_texture()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const float2 P = to_float2x2(transformation) * float2(texel) + transformation[2].xy;
  imageStore(domain_img, texel, texture(input_tx, P));
}

void realize_on_domain_float4x4()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const float2 P = to_float2x2(transformation) * float2(texel) + transformation[2].xy;
  /* Each column of the matrix is stored in one layer of the texture. */
  for (int i = 0; i < 4; i++) {
    imageStore(domain_img, int3(texel, i), texture(input_tx, float3(P, float(i))));
  }
}
