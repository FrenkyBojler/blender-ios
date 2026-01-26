/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* THIS ISN'T USED FOR NOW! WILL BE USED UNTIL SKINNING PRODUCES ACCURATE RESULTS!
 * THEN WE CAN USE THIS AS AN EXTRA LOW QUALITY MODE FOR PERFORMANCE */

float2 unpack_weights_from_uint(uint x)
{
  const float inv65535 = 1.0f / 65535.0f;
  uint w0 = x & 0xFFFFu;
  uint w1 = (x >> 16) & 0xFFFFu;
  return float2(float(w0) * inv65535, float(w1) * inv65535);
}

float4 unpack_weights_from_two_uints(uint a, uint b)
{
  float2 p0 = unpack_weights_from_uint(a);
  float2 p1 = unpack_weights_from_uint(b);
  return float4(p0.x, p0.y, p1.x, p1.y);
}

uvec2 unpack_indices_from_uint(uint x)
{
  uint i0 = x & 0xFFFFu;
  uint i1 = (x >> 16) & 0xFFFFu;
  return uvec2(i0, i1);
}

uvec4 unpack_indices_from_two_uints(uint a, uint b)
{
  uvec2 p0 = unpack_indices_from_uint(a);
  uvec2 p1 = unpack_indices_from_uint(b);
  return uvec4(p0.x, p0.y, p1.x, p1.y);
}

float2 sign_not_zero(float2 v)
{
  return float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}

float3 unpack_octahedral(float2 p)
{
  float3 v = float3(p.x, p.y, 1.0f - abs(p.x) - abs(p.y));
  if (v.z < 0.0f) {
    v.xy = (1.0f - abs(v.yx)) * sign_not_zero(v.xy);
  }
  return normalize(v);
}
