/* SPDX-FileCopyrightText: 2020-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "infos/gpu_srgb_to_framebuffer_space_infos.hh"

SHADER_LIBRARY_CREATE_INFO(gpu_srgb_to_framebuffer_space)

/* Undefine the macro that avoids compilation errors. */
#undef blender_srgb_to_framebuffer_space

/* Raw python shaders don't have create infos and thus don't generate the needed `srgbTarget`
 * uniform automatically. For API compatibility, we sill define this loose uniform, but it will
 * not be parsed by the Metal or Vulkan backend. */
#ifdef GPU_RAW_PYTHON_SHADER
uniform bool srgbTarget = false;
#endif

/* Input is sRGB with Rec.709 primaries. Output is Rec.709 linear or sRGB. */
float4 blender_srgb_to_framebuffer_space(float4 in_color)
{
  /** IMPORTANT: srgbTarget denote that the output is expected to be in __linear__ space. */
  if (srgbTarget) {
    float3 c = max(in_color.rgb, float3(0.0f));
    float3 c1 = c * (1.0f / 12.92f);
    float3 c2 = pow((c + 0.055f) * (1.0f / 1.055f), float3(2.4f));
    in_color.rgb = mix(c1, c2, step(float3(0.04045f), c));
  }
  return in_color;
}

/* Input is scene referred linear. Output is Rec.709 linear or sRGB. */
float4 blender_scene_linear_to_framebuffer_space(float4 in_color)
{
  /** IMPORTANT: srgbTarget denote that the output is expected to be in __linear__ space. */
  if (!srgbTarget) {
    /* TODO(fclem): There should be a matrix transform here to adjust primaries. */
    float3 c = max(in_color.rgb, float3(0.0f));
    float3 c1 = c * 12.92f;
    float3 c2 = 1.055f * pow(c, float3(1.0f / 2.4f)) - 0.055f;
    in_color.rgb = mix(c1, c2, step(float3(0.0031308f), c));
  }
  return in_color;
}
