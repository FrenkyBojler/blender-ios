/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// The fast version can be used if the glsl texture() method produces the desired
// results. Currently this is only used for SAMPLER_NEAREST.
// Note that imat is different as it must translate to texture coordinates.

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float3 uvw = to_float3x3(imat) * float3(texel.x, texel.y, 1.0f);
  if (uvw.z <= 0.0f) {
    imageStore(output_img, texel, float4(0.0f));
  }
  else {
    imageStore(output_img, texel, texture(input_tx, uvw.xy / uvw.z));
  }
}
