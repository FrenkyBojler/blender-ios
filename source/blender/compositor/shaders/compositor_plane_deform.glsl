/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_compositor_sampleRect.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 output_size = float2(imageSize(output_img));

  // change input coordinates from pixels to bounds
  float2 coordinates = (float2(texel) + float2(0.5f)) / output_size;

  float3 uvw = to_float3x3(homography_matrix) * float3(coordinates, 1.0f);

  // Point is at infinity and will be zero when sampled, so early exit.
  // Also negative numbers indicate "behind camera" and should be cropped as well.
  if (uvw.z <= 0.0f) {
    imageStore(output_img, texel, float4(0.0f));
    return;
  }

  float m = 1.0f/uvw.z; // 1/w
  float2 pixels = float2(textureSize(input_tx, 0));
  float2 uv = uvw.xy * m * pixels - 0.5f; // convert from bounds to pixels

  // compute derivative of source location
  float3 m0 = homography_matrix[0].xyz;
  float2 dPdx = (m0.xy - uvw.xy * m0.z * m) * m;
  float3 m1 = homography_matrix[1].xyz;
  float2 dPdy = (m1.xy - uvw.xy * m1.z * m) * m;
  // convert to rectangle and then scale from bounds to pixels
  float2 wh = hypot2(dPdx, dPdy) * pixels / output_size;

  float4 sampled_color = sampleRect(uv, wh);
#if defined(PREMULTIPLY_MASK)
  /* Premultiply the mask value as an alpha. */
  float4 plane_color = sampled_color * texture_load(mask_tx, texel).x;
#else
  float4 plane_color = sampled_color;
#endif

  imageStore(output_img, texel, plane_color);
}
