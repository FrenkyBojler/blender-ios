/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "vk_backbuffer_blit_info.hh"

#include "gpu_shader_colorspace_lib.glsl"

COMPUTE_SHADER_CREATE_INFO(vk_backbuffer_blit)

void main()
{
  ivec2 dst_texel = ivec2(gl_GlobalInvocationID.xy);
  ivec2 src_size = ivec2(imageSize(src_img));
  ivec2 src_texel = ivec2(dst_texel.x, src_size.y - dst_texel.y - 1);

  vec4 color = imageLoad(src_img, ivec2(src_texel));
  color = blender_srgb_to_framebuffer_space(color);
  imageStore(dst_img, ivec2(dst_texel), color);
}
