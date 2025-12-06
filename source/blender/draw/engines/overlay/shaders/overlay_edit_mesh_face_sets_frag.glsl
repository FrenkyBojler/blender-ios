/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_edit_mesh_face_sets)

#include "select_lib.glsl"

void main()
{
  /* Sample rendered color to modulate the face-set overlay so base shading remains visible. */
  float4 base_color = texelFetch(color_render_tx, ivec2(gl_FragCoord.xy), 0);

  if (retopology_enabled) {
    /* For retopology, keep the original premultiplied face-set color (depth-only base may be
     * black). */
    frag_color = face_set_color;
  }
  else {
    /* Un-premultiply inputs to avoid darkening when base color has alpha 0. */
    float fs_alpha = face_set_color.a;
    float3 fs_color = (fs_alpha > 0.0) ? face_set_color.rgb / fs_alpha : face_set_color.rgb;

    /* If base has zero alpha (e.g. depth-only), fall back to white. */
    float3 base_rgb = (base_color.a > 0.0) ? base_color.rgb / base_color.a : float3(1.0);

    /* Modulate base shading by the face-set color; mix keeps base when opacity < 1. */
    float3 mod_rgb = base_rgb * fs_color;
    float3 out_rgb = mix(base_rgb, mod_rgb, fs_alpha);

    /* Output premultiplied. */
    float3 out_rgb_pm = out_rgb * fs_alpha;
    float overlay_alpha = fs_alpha;
    float3 overlay_rgb = out_rgb_pm;
    frag_color = float4(overlay_rgb, overlay_alpha);
  }

  select_id_output(select_id);
}
