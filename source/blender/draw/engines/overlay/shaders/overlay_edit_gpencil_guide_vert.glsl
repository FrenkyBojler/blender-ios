/* SPDX-FileCopyrightText: 2020-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "common_view_clipping_lib.glsl"
#include "draw_model_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
  gl_Position = drw_point_world_to_homogenous(pPosition);
  finalColor = pColor;
  gl_PointSize = pSize;

  view_clipping_distances(pPosition);
}
