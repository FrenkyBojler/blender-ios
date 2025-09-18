/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_extra_info.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_dome_hdr)

#include "overlay_common_lib.glsl"
#include "select_lib.glsl"

void main()
{
  float4 hdr_color;
  
  /* Sample the HDR texture using the calculated UV coordinates */
  hdr_color = texture(hdr_texture, uv_coords);
  
  /* Check if we have valid HDR data (not black) */
  float hdr_luminance = dot(hdr_color.rgb, float3(0.299f, 0.587f, 0.114f));
  
  /* Use the texture if it has content, otherwise show debug pattern */
  if (hdr_luminance > 0.001f || has_hdr > 0.0f) {
    /* Extract HDR parameters: strength, gamma, exposure */
    float hdr_strength = hdr_params.x;
    float hdr_gamma = hdr_params.y;
    float light_exposure = hdr_params.z;
    
    /* Apply strength (intensity multiplier) */
    float3 strengthened_color = hdr_color.rgb * hdr_strength;
    
    /* Apply exposure adjustment */
    float3 exposed_color = strengthened_color * pow(2.0f, light_exposure);
    
    /* Enhanced Reinhard tone mapping for HDR */
    float3 mapped_color = exposed_color / (float3(1.0f) + exposed_color);
    
    /* Apply gamma correction */
    float3 gamma_corrected = pow(mapped_color, float3(1.0f / hdr_gamma));
    
    /* Mix with wireframe color for selection feedback */
    float3 final_rgb = gamma_corrected;
    if (final_color.r > 0.9f && final_color.g < 0.6f && final_color.b < 0.6f) {
      /* Object is selected (reddish wireframe) - add subtle red tint */
      final_rgb = mix(gamma_corrected, final_color.rgb, 0.1f);
    }
    
    /* Set final color without transparency */
    frag_color = float4(final_rgb, 1.0f);
  }
  else {
    /* Enhanced debug pattern when no HDR texture is available */
    /* Create a more sophisticated debug pattern to indicate missing texture */
    float checker = mod(floor(uv_coords.x * 8.0f) + floor(uv_coords.y * 8.0f), 2.0f);
    float3 color1 = float3(0.2f, 0.3f, 0.5f); /* Blue-ish */
    float3 color2 = float3(0.5f, 0.4f, 0.3f); /* Orange-ish */
    float3 uv_debug_color = mix(color1, color2, checker);
    
    /* Add gradient overlay for depth perception */
    uv_debug_color *= 0.05f + 0.05f * (uv_coords.y);

    frag_color = float4(uv_debug_color, 1.0f);
  }
  
  /* No line output for solid dome rendering */
  line_output = float4(0.0f);
  
  select_id_output(select_id);
}