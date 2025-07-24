/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  imageStore(output_img, int2(gl_GlobalInvocationID.xy), texture_load(input_tx, texel));
}
