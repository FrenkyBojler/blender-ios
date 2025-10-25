/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * The fast version can be used if the glsl texture() method produces the desired
 * results. This is true for SAMPLER_NEAREST. It also works for SAMPLER_BOX
 * if the derivative is 1 or less everywhere, and in some other cases.
 * Note that inverse_matrix is different as it must translate to texture coordinates.
 */

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  float2 uv = to_float2x2(inverse_matrix) * float2(texel) + inverse_matrix[2].xy;

  imageStore(domain_img, texel, texture(input_tx, uv, 0));
}
