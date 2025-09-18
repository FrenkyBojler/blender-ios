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
  
  /* Check if HDR texture is available */
  if (has_hdr > 0.5f) {
    /* Sample the HDR texture using the calculated UV coordinates */
    hdr_color = texture(hdr_texture, uv_coords);
    
    /* Advanced HDR tone mapping for better visualization */
    /* Reinhard tone mapping with exposure control */
    float exposure = 1.5f; /* Slightly increase exposure for better visibility */
    float3 mapped_color = hdr_color.rgb * exposure;
    
    /* Enhanced Reinhard tone mapping for HDR */
    mapped_color = mapped_color / (float3(1.0f) + mapped_color);
    
    /* Apply gamma correction for display */
    float gamma = 2.2f;
    mapped_color = pow(mapped_color, float3(1.0f / gamma));
    
    /* Enhance contrast slightly for better visibility in viewport */
    mapped_color = mapped_color * 1.1f - 0.05f;
    mapped_color = clamp(mapped_color, 0.0f, 1.0f);
    
    /* Mix with wireframe color for selection feedback */
    float3 final_rgb = mapped_color;
    if (final_color.r > 0.9f && final_color.g < 0.6f && final_color.b < 0.6f) {
      /* Object is selected (reddish wireframe) - add subtle red tint */
      final_rgb = mix(mapped_color, final_color.rgb, 0.1f);
    }
    
    /* Set final color with proper alpha blending for overlay */
    frag_color = float4(final_rgb, final_color.a * 0.85f);
  }
  else {
    /* Enhanced debug pattern when no HDR texture is available */
    /* Create a more sophisticated debug pattern to indicate missing texture */
    float checker = mod(floor(uv_coords.x * 8.0f) + floor(uv_coords.y * 8.0f), 2.0f);
    float3 color1 = float3(0.2f, 0.3f, 0.5f); /* Blue-ish */
    float3 color2 = float3(0.5f, 0.4f, 0.3f); /* Orange-ish */
    float3 uv_debug_color = mix(color1, color2, checker);
    
    /* Add gradient overlay for depth perception */
    uv_debug_color *= 0.5f + 0.5f * (uv_coords.y);
    
    /* Add animated pulse to indicate it's a placeholder */
    /* Note: We don't have time uniform, so using UV-based animation */
    float pulse = 0.8f + 0.2f * sin(uv_coords.x * 10.0f + uv_coords.y * 10.0f);
    uv_debug_color *= pulse;
    
    frag_color = float4(uv_debug_color, final_color.a * 0.9f);
  }
  
  /* No line output for solid dome rendering */
  line_output = float4(0.0f);
  
  select_id_output(select_id);
}