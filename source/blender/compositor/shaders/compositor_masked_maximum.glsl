/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_math_vector_lib.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  float4 size = texture_load(input_size_tx, texel);

  imageStore(output_img,
             texel,
             texture_load(input_image_tx, texel) + size.x + size.y + size.z +
                 texture_load(input_roundness_tx, texel) + texture_load(input_falloff_tx, texel));
}
