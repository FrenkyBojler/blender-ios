/* SPDX-FileCopyrightText: 2018-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_curves_info.hh"

VERTEX_SHADER_CREATE_INFO(draw_curves_test)

#include "draw_curves_lib.glsl"

void main()
{
#ifdef GPU_VERTEX_SHADER
  result_buf[gl_VertexID] = gl_VertexID;
#endif
}
