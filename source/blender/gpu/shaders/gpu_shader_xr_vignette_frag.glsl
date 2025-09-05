/* SPDX-FileCopyrightText: 2017-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_vignette_info.hh"

#include "gpu_shader_math_base_lib.glsl"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_vignette)

/* Adapted from overlay_background_frag.glsl */
float dither()
{
  /* 4x4 bayer matrix prepared for 8bit UNORM precision error. */
  /* NOTE(Metal): Declaring constant array in function scope to avoid increasing local shader
   * memory pressure. */
#define P(x) (((x + 0.5f) * (1.0f / 16.0f) - 0.5f) * (1.0f / 255.0f))
  constexpr float4 dither_mat4x4[4] = float4_array(float4(P(0.0f), P(8.0f), P(2.0f), P(10.0f)),
                                                   float4(P(12.0f), P(4.0f), P(14.0f), P(6.0f)),
                                                   float4(P(3.0f), P(11.0f), P(1.0f), P(9.0f)),
                                                   float4(P(15.0f), P(7.0f), P(13.0f), P(5.0f)));
#undef P
  int2 co = int2(gl_FragCoord.xy) % 4;
  return dither_mat4x4[co.x][co.y];
}

/* Adapted from overlay_background_frag.glsl */
float3 get_background_color(float2 screen_uv)
{
  float3 bg_col;
  float3 col_high;
  float3 col_low;

  OVERLAY_BackgroundType type = OVERLAY_BackgroundType(background_type);
  switch (type) {
    case BG_SOLID:
      bg_col = background.rgb;
      break;
    case BG_GRADIENT:
      /* XXX do interpolation in a non-linear space to have a better visual result. */
      col_high = pow(background.rgb, float3(1.0f / 2.2f));
      col_low = pow(background_gradient.rgb, float3(1.0f / 2.2f));
      bg_col = mix(col_low, col_high, screen_uv.y);
      /* Convert back to linear. */
      bg_col = pow(bg_col, float3(2.2f));
      /* Dither to hide low precision buffer. (Could be improved) */
      bg_col += dither();
      break;
    case BG_RADIAL: {
      /* Do interpolation in a non-linear space to have a better visual result. */
      col_high = pow(background.rgb, float3(1.0f / 2.2f));
      col_low = pow(background_gradient.rgb, float3(1.0f / 2.2f));

      float2 uv_n = screen_uv - 0.5f;
      bg_col = mix(col_high, col_low, length(uv_n) * M_SQRT2);

      /* Convert back to linear. */
      bg_col = pow(bg_col, float3(2.2f));
      /* Dither to hide low precision buffer. (Could be improved). */
      bg_col += dither();
      break;
    }
    default:
      bg_col = background.rgb;
      break;
  }

  return bg_col;
}

void main()
{
  const float2 uv = gl_FragCoord.xy / viewportSize;
  const float dist = length(uv - float2(0.5));

  // Create vignette factor using smoothstep
  const float vignette = smoothstep(aperture, aperture + falloff, dist);

  // Output the vignette color with fade applied
  fragColor = vec4(get_background_color(uv), background.a * vignette);
}
