/* SPDX-FileCopyrightText: 2020-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#define OVERLAY_UV_LINE_STYLE_OUTLINE 0
#define OVERLAY_UV_LINE_STYLE_DASH 1
#define OVERLAY_UV_LINE_STYLE_BLACK 2
#define OVERLAY_UV_LINE_STYLE_WHITE 3
#define OVERLAY_UV_LINE_STYLE_SHADOW 4

#ifdef COMMON_GLOBALS_LIB
float mul_project_m4_v3_zfac(vec3 co)
{
  vec3 vP = (ViewMatrix * vec4(co, 1.0)).xyz;
  return pixelFac * ((ProjectionMatrix[0][3] * vP.x) + (ProjectionMatrix[1][3] * vP.y) +
                     (ProjectionMatrix[2][3] * vP.z) + ProjectionMatrix[3][3]);
}
#endif

/* Not the right place but need to be common to all overlay's.
mat4 extract_matrix_packed_data(mat4 mat, out vec4 dataA, out vec4 dataB)
{
  const float div = 1.0 / 255.0;
  int a = int(mat[0][3]);
  int b = int(mat[1][3]);
  int c = int(mat[2][3]);
  int d = int(mat[3][3]);
  dataA = vec4(a & 0xFF, a >> 8, b & 0xFF, b >> 8) * div;
  dataB = vec4(c & 0xFF, c >> 8, d & 0xFF, d >> 8) * div;
  mat[0][3] = mat[1][3] = mat[2][3] = 0.0;
  mat[3][3] = 1.0;
  return mat;
}

/* Same here, Not the right place but need to be common to all overlay's.
/* edge_start and edge_pos needs to be in the range [0..sizeViewport]. */
vec4 pack_line_data(vec2 frag_co, vec2 edge_start, vec2 edge_pos)
{
  vec2 edge = edge_start - edge_pos;
  float len = length(edge);
  if (len > 0.0) {
    edge /= len;
    vec2 perp = vec2(-edge.y, edge.x);
    float dist = dot(perp, frag_co - edge_start);
    /* Add 0.1 to differentiate with cleared pixels. */
    return vec4(perp * 0.5 + 0.5, dist * 0.25 + 0.5 + 0.1, 1.0);
  }
  else {
    /* Default line if the origin is perfectly aligned with a pixel. */
    return vec4(1.0, 0.0, 0.5 + 0.1, 1.0);
  }
}
