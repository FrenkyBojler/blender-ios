/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "vk_backbuffer_blit_info.hh"

COMPUTE_SHADER_CREATE_INFO(vk_backbuffer_blit_shared)

#define M1 0.1593017578125
#define M2 78.84375
#define C1 0.8359375
#define C2 18.8515625
#define C3 18.6875

void main()
{
  ivec2 dst_texel = ivec2(gl_GlobalInvocationID.xy);
  ivec2 src_size = ivec2(imageSize(src_img));
  ivec2 src_texel = ivec2(dst_texel.x, src_size.y - dst_texel.y - 1);
  vec4 color = imageLoad(src_img, ivec2(src_texel));

#if defined(COLOR_SPACE_EXTENDED_SRGB_LINEAR)

  /*
   * Convert from extended sRGB non-linear to linear.
   *
   * Preserves negative wide gamut values with sign/abs.
   * Gamma 2.2 is used instead of the sRGB piecewise transfer function, because
   * most SDR sRGB displays decode with gamma 2.2, and that's what we are trying
   * to match.
   */
  color.rgb = sign(color.rgb) * pow(abs(color.rgb), vec3(2.2f)) * sdr_scale;

#elif defined(COLOR_SPACE_HDR10_ST2084)

  /*
   * Convert from sRGB non-linear to sRGB linear to HDR10 (BT2020) using SMPTE ST2084 PQ.
   *
   * This corresponds to VK_COLOR_SPACE_HDR10_ST2084_EXT:
   * https://registry.khronos.org/vulkan/specs/latest/man/html/VkColorSpaceKHR.html
   * For more details see 13.6 BT.2100 PQ transfer functions from:
   * https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.pdf
   */

  /* Convert from sRGB non-linear to linear. */
  vec3 color_srgb_nonlinear = abs(color.rgb);
  // vec3 color_linear = pow(color_srgb_nonlinear, vec3(2.2f));
  vec3 color_linear = mix(pow((color_srgb_nonlinear + vec3(0.055)) / 1.055, vec3(2.4)),
                          color_srgb_nonlinear / 12.92,
                          lessThanEqual(color_srgb_nonlinear, vec3(0.04045)));

  /* Convert from linear to HDR10 (BT2020) using SMPTE ST2084 PQ. */
  vec3 Y = color_linear * 1e-4;
  vec3 pow_base = (vec3(C1) + C2 * pow(Y, vec3(M1))) / (vec3(1.0) + C3 * pow(Y, vec3(M1)));
  color.rgb = sign(color.rgb) * pow(pow_base, vec3(M2)) * sdr_scale;

#else

#  error Unknown color space

#endif

  imageStore(dst_img, ivec2(dst_texel), color);
}
