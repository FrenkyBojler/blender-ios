/* SPDX-FileCopyrightText: 2016-2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_xr_raycast_info.hh"

VERTEX_SHADER_CREATE_INFO(gpu_shader_xr_raycast)

vec3 catmullRom(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t)
{
  float t2 = t * t;
  float t3 = t2 * t;
  return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

vec3 getControlPoint(int idx)
{
  idx = clamp(idx, 0, controlPointCount - 1);
  return controlPoints[idx].xyz;
}

void main()
{
  int sampleIdx = gl_VertexID >> 1;
  float side = ((gl_VertexID & 1) != 0) ? -1.0 : 1.0;

  int segmentIdx = sampleIdx / samplesPerSegment;
  int sampleInSegment = sampleIdx - segmentIdx * samplesPerSegment;
  float t = float(sampleInSegment) / float(samplesPerSegment - 1);

  segmentIdx = clamp(segmentIdx, 0, max(0, controlPointCount - 2));

  vec3 p0 = getControlPoint(segmentIdx - 1);
  vec3 p1 = getControlPoint(segmentIdx + 0);
  vec3 p2 = getControlPoint(segmentIdx + 1);
  vec3 p3 = getControlPoint(segmentIdx + 2);

  vec3 pos = catmullRom(p0, p1, p2, p3, t) + 0.5 * width * side * rightVector;

  gl_Position = ModelViewProjectionMatrix * vec4(pos, 1.0);
}
