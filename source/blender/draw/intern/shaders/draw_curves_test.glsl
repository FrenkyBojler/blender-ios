/* SPDX-FileCopyrightText: 2018-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_curves_info.hh"

VERTEX_SHADER_CREATE_INFO(draw_curves_test)

#include "draw_curves_lib.glsl"

void main()
{
#ifdef GPU_VERTEX_SHADER
  curves::Point pt = curves::point_get(uint(gl_VertexID));

  result_pos_buf[gl_VertexID] = pt.P.x;
  result_indices_buf[gl_VertexID].x = pt.point_id;
  result_indices_buf[gl_VertexID].y = pt.curve_id;
  result_indices_buf[gl_VertexID].z = pt.curve_segment;
#endif
}
