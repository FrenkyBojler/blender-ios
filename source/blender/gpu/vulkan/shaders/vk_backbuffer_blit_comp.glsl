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
   * Convert from Rec.709 sRGB to HDR10 (BT2020) using SMPTE ST2084 PQ.
   *
   * This corresponds to VK_COLOR_SPACE_HDR10_ST2084_EXT:
   * https://registry.khronos.org/vulkan/specs/latest/man/html/VkColorSpaceKHR.html
   */

  /* Convert from Rec.709 sRGB to Rec.709 linear: sRGB EOTF from
   * https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.pdf#page=146 */
  vec3 color_rec709_srgb = abs(color.rgb);
  vec3 color_rec709_linear = mix(pow((color_rec709_srgb + vec3(0.055)) / 1.055, vec3(2.4)),
                                 color_rec709_srgb / 12.92,
                                 lessThanEqual(color_rec709_srgb, vec3(0.04045)));
  color_rec709_linear = sign(color.rgb) * color_rec709_linear;

  /* Convert from Rec.709 linear to Rec.2020 linear. See section 14.12 from
   * https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.pdf#page=176 */
  const mat3 rec709_linear_to_rec2020_linear_mat = mat3(0.62740390518651268,
                                                        0.069097276721845929,
                                                        0.01639143846516198,
                                                        0.32928305625679771,
                                                        0.91954038221065537,
                                                        0.088013328319648046,
                                                        0.043313080232962237,
                                                        0.011362275805175567,
                                                        0.89559526138792178);
  vec3 color_rec2020_linear = rec709_linear_to_rec2020_linear_mat * color_rec709_linear;
  color_rec2020_linear = max(color_rec2020_linear, vec3(0.0));

  /* Convert from Rec.2020 linear to HDR10 (BT2020) using SMPTE ST2084 PQ.
   * sdr_scale value of 1 corresponds to 80 nits, so multiply with 80 / 10000.0. See:
   * https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.pdf#page=160 */
  vec3 Y = color_rec2020_linear * sdr_scale * 8e-3;
  vec3 Y_pow_M1 = pow(Y, vec3(M1));
  color.rgb = pow((vec3(C1) + C2 * Y_pow_M1) / (vec3(1.0) + C3 * Y_pow_M1), vec3(M2));

#else

#  error Unknown color space

#endif

  imageStore(dst_img, ivec2(dst_texel), color);
}
