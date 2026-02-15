/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/eevee_film_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_film_frag)

#include "eevee_film_lib.glsl"

void main()
{
  int2 texel_film = int2(gl_FragCoord.xy) - uniform_buf.film.offset;
  float out_depth;

  if (uniform_buf.film.display_only) {
    out_depth = imageLoadFast(depth_img, texel_film).r;

    if (display_id == -1) {
      out_color = texelFetch(in_combined_tx, texel_film, 0);
    }
    else if (uniform_buf.film.display_storage_type == PASS_STORAGE_VALUE) {
      out_color.rgb = imageLoadFast(value_accum_img, int3(texel_film, display_id)).rrr;
      out_color.a = 1.0f;
    }
    else if (uniform_buf.film.display_storage_type == PASS_STORAGE_COLOR) {
      out_color = imageLoadFast(color_accum_img, int3(texel_film, display_id));
    }
    else /* PASS_STORAGE_CRYPTOMATTE */ {
      out_color = cryptomatte_false_color(
          imageLoadFast(cryptomatte_img, int3(texel_film, display_id)).r);
    }

    /* Apply film exposure to combined and light passes. */
    bool use_exposure = (display_id == -1) || (display_id == uniform_buf.film.diffuse_light_id) ||
                        (display_id == uniform_buf.film.specular_light_id) ||
                        (display_id == uniform_buf.film.volume_light_id) ||
                        (display_id == uniform_buf.film.emission_id) ||
                        (display_id == uniform_buf.film.environment_id) ||
                        (display_id == uniform_buf.film.shadow_id) ||
                        (display_id == uniform_buf.film.transparent_id);
    if (use_exposure) {
      out_color.rgb *= uniform_buf.film.film_exposure;
      if (uniform_buf.film.use_camera_responsivity != 0) {
        float3x3 resp = float3x3(uniform_buf.film.camera_to_scene_linear_row0.xyz,
                                 uniform_buf.film.camera_to_scene_linear_row1.xyz,
                                 uniform_buf.film.camera_to_scene_linear_row2.xyz);
        out_color.rgb = out_color.rgb * resp;
      }
    }
  }
  else {
    film_process_data(texel_film, out_color, out_depth);
  }

  gl_FragDepth = drw_depth_view_to_screen(-out_depth);

  gl_FragDepth = film_display_depth_amend(texel_film, gl_FragDepth);
}
