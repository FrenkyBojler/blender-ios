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
  // We output TWO verts per sample: (left, right)
  int vertex = gl_VertexID;
  int sampleIx = vertex >> 1;                   // /2
  int sideBit = vertex & 1;                     // 0 or 1
  float side = mix(-1.0, 1.0, float(sideBit));  // -1 = left, +1 = right

  // Map sampleIx -> segment index & local t in [0,1]
  int seg = sampleIx / samplesPerSegment;  // 0..(N-2)
  int sInSeg = sampleIx - seg * samplesPerSegment;
  float t = float(sInSeg) / float(samplesPerSegment - 1);

  seg = clamp(seg, 0, max(0, controlPointCount - 2));

  // Control points p0..p3 around segment [p1,p2]
  vec3 p0 = getControlPoint(seg - 1);
  vec3 p1 = getControlPoint(seg + 0);
  vec3 p2 = getControlPoint(seg + 1);
  vec3 p3 = getControlPoint(seg + 2);

  // Position on curve and tangent
  vec3 pos = catmullRom(p0, p1, p2, p3, t);

  float halfW = 0.5 * width;
  pos += side * halfW * rightVector;

  gl_Position = ModelViewProjectionMatrix * vec4(pos, 1.0);
}
