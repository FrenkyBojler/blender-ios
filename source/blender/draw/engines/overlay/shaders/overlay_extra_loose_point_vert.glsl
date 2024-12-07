/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "common_view_clipping_lib.glsl"
#include "draw_model_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
#ifdef OVERLAY_NEXT
  finalColor = vertex_color;
#else
  /* Extract data packed inside the unused mat4 members. */
  mat4 obmat = ModelMatrix;
  finalColor = vec4(obmat[0][3], obmat[1][3], obmat[2][3], obmat[3][3]);
#endif

  vec3 world_pos = (ModelMatrix * vec4(pos, 1.0)).xyz;
  gl_Position = drw_point_world_to_homogenous(world_pos);

  gl_PointSize = sizeVertex * 2.0;

  view_clipping_distances(world_pos);
}
