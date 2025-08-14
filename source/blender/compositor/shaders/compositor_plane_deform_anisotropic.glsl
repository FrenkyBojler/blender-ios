/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 output_size = float2(imageSize(output_img));

  float2 coordinates = (float2(texel) + float2(0.5f)) / output_size;

  float3 uvw = to_float3x3(homography_matrix) * float3(coordinates, 1.0f);

  // Point is at infinity and will be zero when sampled, so early exit.
  // Also negative numbers indicate "behind camera" and should be cropped as well.
  if (uvw.z <= 0.0f) {
    imageStore(output_img, texel, float4(0.0f));
    return;
  }

  float m = 1.0f/uvw.z; // 1/w
  float2 uv = uvw.xy * m;

  // compute derivative of source location per output pixel
  float3 m0 = homography_matrix[0].xyz;
  float2 dPdx = (m0.xy - uvw.xy * m0.z * m) * m / output_size.x;
  float3 m1 = homography_matrix[1].xyz;
  float2 dPdy = (m1.xy - uvw.xy * m1.z * m) * m / output_size.y;

  float4 sampled_color = textureGrad(input_tx, uv, dPdx, dPdy);

  /* Premultiply the mask value as an alpha. */
  float4 plane_color = sampled_color * texture_load(mask_tx, texel).x;

  imageStore(output_img, texel, plane_color);
}
