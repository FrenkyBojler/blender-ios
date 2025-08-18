/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "vk_backbuffer_blit_info.hh"

COMPUTE_SHADER_CREATE_INFO(vk_backbuffer_blit)

void main()
{
  ivec2 dst_texel = ivec2(gl_GlobalInvocationID.xy);
  ivec2 src_size = ivec2(imageSize(src_img));
  ivec2 src_texel = ivec2(dst_texel.x, src_size.y - dst_texel.y - 1);
  vec4 color = imageLoad(src_img, ivec2(src_texel));
  /* TODO: Validate for correctness. On windows pixels containing negative values will can become black. */
  color.rgb = sign(color.rgb) * pow(color.rgb, vec3(2.2f));
  imageStore(dst_img, ivec2(dst_texel), color);
}