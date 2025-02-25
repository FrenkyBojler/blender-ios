/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
  finalColor = colorVertexSelect;
  float radius = pos_rad.w;

  vec3 world_pos = drw_point_object_to_world(pos_rad.xyz);
  gl_Position = drw_point_world_to_homogenous(world_pos);

  /* Small offset in Z */
  gl_Position.z -= 3e-4;

  gl_PointSize = sizeVertex * 2.0 * radius;

  view_clipping_distances(world_pos);
}
