/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_edit_mesh_face_sets)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "overlay_common_lib.glsl"

void main()
{
  float3 world_pos = drw_point_object_to_world(pos);
  float3 view_pos = drw_point_world_to_view(world_pos);
  gl_Position = drw_point_view_to_homogenous(view_pos);

  /* Use proper Z-offset like retopology shader */
  gl_Position.z += get_homogenous_z_offset(
      drw_view().winmat, view_pos.z, gl_Position.w, retopology_offset);

  /* Extract face set color from fset_color */
  /* Check if this is default face set marker (alpha=0, RGB=1.0) - use theme color directly */
  if (fset_color.a == 0.0 && all(equal(fset_color.rgb, float3(1.0)))) {
    /* Use theme color for default face set - this updates immediately when theme changes */
    face_set_color = theme.colors.face_sets_default;
  }
  else {
    face_set_color = fset_color;
  }
  /* Apply face sets opacity for Edit Mode Face Sets overlay */
  face_set_color.a = face_set_color.a * face_sets_opacity;
  /* Apply premultiplication in vertex shader for better performance */
  face_set_color.rgb *= face_set_color.a;
  
  /* Apply fake shading for lit mode */
#ifdef FAKE_SHADING
  float3 view_normal = normalize(drw_normal_object_to_view(nor));
  color_fac = abs(dot(view_normal, light_dir));
  color_fac = color_fac * 0.9f + 0.1f;
#else
  color_fac = 1.0f;
#endif
  view_clipping_distances(world_pos);
}
