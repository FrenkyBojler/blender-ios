/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"

#if defined(PREMULTIPLY_MASK)
float2 hypot2(float2 a, float2 b)
{
  //return float2(length(float2(a.x,b.x)), length(float2(a.y,b.y)));
  return sqrt(float2(a.x*a.x+b.x*b.x, a.y*a.y+b.y*b.y));
}
#endif

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 output_size = float2(imageSize(output_img));

  float3 uvw = to_float3x3(imat) * float3(texel.x, texel.y, 1.0f);

  // Point is at infinity and will be zero when sampled, so early exit.
  // Also negative numbers indicate "behind camera" and should be cropped as well.
  if (uvw.z <= 0.0f) {
    imageStore(output_img, texel, float4(0.0f));
    return;
  }

  float iw = 1.0f/uvw.z; // 1/w

  // compute derivative of source location
  float3 m0 = imat[0].xyz;
  float2 dPdx = (m0.xy - uvw.xy * m0.z * iw) * iw;
  float3 m1 = imat[1].xyz;
  float2 dPdy = (m1.xy - uvw.xy * m1.z * iw) * iw;

  float m = 1;

  // antialias the horizon line
  float dw = length(float2(m0.z, m1.z));
  if (dw > uvw.z) m = uvw.z / dw;

  float2 uv = uvw.xy * iw;

#if defined(PREMULTIPLY_MASK)
  // convert derivatives to rectangle
  float2 wh = hypot2(dPdx, dPdy);
  float2 pixels = float2(textureSize(input_tx, 0));
  float2 mm = clamp(min(uv + 0.5f, pixels - uv - 0.5f) / wh + 0.5f, 0, 1); // coverage of wh by image
  if (m < 1) mm = float2(0); // remove artifacts at horizon
  // mm = max(mm, mask_mult); // keep unclipped sides (nyi for anisotropic)
  m *= mm.x * mm.y;
  if (m <= 0) {
    imageStore(output_img, texel, float4(0.0f));
    return;
  }
#endif

  float2 ipixels = 1 / float2(textureSize(input_tx, 0));
  imageStore(output_img, texel,
             textureGrad(input_tx, (uv + 0.5f)*ipixels, dPdx*ipixels, dPdy*ipixels) * m);
}
