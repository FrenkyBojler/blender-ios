/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/svm/fractal_noise.h"
#include "kernel/svm/node_types.h"
#include "kernel/svm/util.h"

CCL_NAMESPACE_BEGIN

enum NodeMxNoiseType {
  NODE_MX_NOISE_PERLIN = 0,
  NODE_MX_NOISE_FRACTAL = 1,
  NODE_MX_NOISE_CELL = 2,
  NODE_MX_NOISE_WORLEY = 3,
  NODE_MX_NOISE_UNIFIED = 4,
};

ccl_device_inline float mx_select(const bool b, const float t, const float f)
{
  return b ? t : f;
}

ccl_device_inline float mx_negate_if(const float val, const bool b)
{
  return b ? -val : val;
}

ccl_device_inline float mx_floorfrac(const float x, ccl_private int *i)
{
  *i = floor_to_int(x);
  return x - float(*i);
}

ccl_device_inline float mx_bilerp(const float v0,
                                  const float v1,
                                  const float v2,
                                  const float v3,
                                  const float s,
                                  const float t)
{
  const float s1 = 1.0f - s;
  return (1.0f - t) * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s);
}

ccl_device_inline float3 mx_bilerp(const float3 v0,
                                   const float3 v1,
                                   const float3 v2,
                                   const float3 v3,
                                   const float s,
                                   const float t)
{
  const float s1 = 1.0f - s;
  return (1.0f - t) * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s);
}

ccl_device_inline float mx_trilerp(const float v0,
                                   const float v1,
                                   const float v2,
                                   const float v3,
                                   const float v4,
                                   const float v5,
                                   const float v6,
                                   const float v7,
                                   const float s,
                                   const float t,
                                   const float r)
{
  const float s1 = 1.0f - s;
  const float t1 = 1.0f - t;
  const float r1 = 1.0f - r;
  return r1 * (t1 * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s)) +
         r * (t1 * (v4 * s1 + v5 * s) + t * (v6 * s1 + v7 * s));
}

ccl_device_inline float3 mx_trilerp(const float3 v0,
                                    const float3 v1,
                                    const float3 v2,
                                    const float3 v3,
                                    const float3 v4,
                                    const float3 v5,
                                    const float3 v6,
                                    const float3 v7,
                                    const float s,
                                    const float t,
                                    const float r)
{
  const float s1 = 1.0f - s;
  const float t1 = 1.0f - t;
  const float r1 = 1.0f - r;
  return r1 * (t1 * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s)) +
         r * (t1 * (v4 * s1 + v5 * s) + t * (v6 * s1 + v7 * s));
}

ccl_device_inline float mx_gradient_float(const uint hash, const float x, const float y)
{
  const uint h = hash & 7u;
  const float u = mx_select(h < 4u, x, y);
  const float v = 2.0f * mx_select(h < 4u, y, x);
  return mx_negate_if(u, bool(h & 1u)) + mx_negate_if(v, bool(h & 2u));
}

ccl_device_inline float mx_gradient_float(const uint hash,
                                          const float x,
                                          const float y,
                                          const float z)
{
  const uint h = hash & 15u;
  const float u = mx_select(h < 8u, x, y);
  const float v = mx_select(h < 4u, y, mx_select((h == 12u) || (h == 14u), x, z));
  return mx_negate_if(u, bool(h & 1u)) + mx_negate_if(v, bool(h & 2u));
}

struct MxHash3 {
  uint x, y, z;
};

ccl_device_inline float3 mx_gradient_vec3(const MxHash3 hash, const float x, const float y)
{
  return make_float3(mx_gradient_float(hash.x, x, y),
                     mx_gradient_float(hash.y, x, y),
                     mx_gradient_float(hash.z, x, y));
}

ccl_device_inline float3 mx_gradient_vec3(const MxHash3 hash,
                                          const float x,
                                          const float y,
                                          const float z)
{
  return make_float3(mx_gradient_float(hash.x, x, y, z),
                     mx_gradient_float(hash.y, x, y, z),
                     mx_gradient_float(hash.z, x, y, z));
}

ccl_device_inline uint mx_rotl32(const uint x, const int k)
{
  return (x << k) | (x >> (32 - k));
}

ccl_device_inline void mx_bjmix(ccl_private uint *a, ccl_private uint *b, ccl_private uint *c)
{
  *a -= *c;
  *a ^= mx_rotl32(*c, 4);
  *c += *b;
  *b -= *a;
  *b ^= mx_rotl32(*a, 6);
  *a += *c;
  *c -= *b;
  *c ^= mx_rotl32(*b, 8);
  *b += *a;
  *a -= *c;
  *a ^= mx_rotl32(*c, 16);
  *c += *b;
  *b -= *a;
  *b ^= mx_rotl32(*a, 19);
  *a += *c;
  *c -= *b;
  *c ^= mx_rotl32(*b, 4);
  *b += *a;
}

ccl_device_inline uint mx_bjfinal(uint a, uint b, uint c)
{
  c ^= b;
  c -= mx_rotl32(b, 14);
  a ^= c;
  a -= mx_rotl32(c, 11);
  b ^= a;
  b -= mx_rotl32(a, 25);
  c ^= b;
  c -= mx_rotl32(b, 16);
  a ^= c;
  a -= mx_rotl32(c, 4);
  b ^= a;
  b -= mx_rotl32(a, 14);
  c ^= b;
  c -= mx_rotl32(b, 24);
  return c;
}

ccl_device_inline float mx_fade(const float t)
{
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

ccl_device_inline MxHash3 mx_hash_vec3(const int x, const int y)
{
  const uint h = hash_uint2(uint(x), uint(y));
  MxHash3 result = {h & 0xffu, (h >> 8) & 0xffu, (h >> 16) & 0xffu};
  return result;
}

ccl_device_inline MxHash3 mx_hash_vec3(const int x, const int y, const int z)
{
  const uint h = hash_uint3(uint(x), uint(y), uint(z));
  MxHash3 result = {h & 0xffu, (h >> 8) & 0xffu, (h >> 16) & 0xffu};
  return result;
}

ccl_device float3 mx_perlin_noise_vec3(const float2 p)
{
  int X, Y;
  const float fx = mx_floorfrac(p.x, &X);
  const float fy = mx_floorfrac(p.y, &Y);
  const float u = mx_fade(fx);
  const float v = mx_fade(fy);
  return 0.6616f *
         mx_bilerp(mx_gradient_vec3(mx_hash_vec3(X, Y), fx, fy),
                   mx_gradient_vec3(mx_hash_vec3(X + 1, Y), fx - 1.0f, fy),
                   mx_gradient_vec3(mx_hash_vec3(X, Y + 1), fx, fy - 1.0f),
                   mx_gradient_vec3(mx_hash_vec3(X + 1, Y + 1), fx - 1.0f, fy - 1.0f),
                   u,
                   v);
}

ccl_device float3 mx_perlin_noise_vec3(const float3 p)
{
  int X, Y, Z;
  const float fx = mx_floorfrac(p.x, &X);
  const float fy = mx_floorfrac(p.y, &Y);
  const float fz = mx_floorfrac(p.z, &Z);
  const float u = mx_fade(fx);
  const float v = mx_fade(fy);
  const float w = mx_fade(fz);
  return 0.9820f *
         mx_trilerp(mx_gradient_vec3(mx_hash_vec3(X, Y, Z), fx, fy, fz),
                    mx_gradient_vec3(mx_hash_vec3(X + 1, Y, Z), fx - 1.0f, fy, fz),
                    mx_gradient_vec3(mx_hash_vec3(X, Y + 1, Z), fx, fy - 1.0f, fz),
                    mx_gradient_vec3(mx_hash_vec3(X + 1, Y + 1, Z), fx - 1.0f, fy - 1.0f, fz),
                    mx_gradient_vec3(mx_hash_vec3(X, Y, Z + 1), fx, fy, fz - 1.0f),
                    mx_gradient_vec3(mx_hash_vec3(X + 1, Y, Z + 1), fx - 1.0f, fy, fz - 1.0f),
                    mx_gradient_vec3(mx_hash_vec3(X, Y + 1, Z + 1), fx, fy - 1.0f, fz - 1.0f),
                    mx_gradient_vec3(mx_hash_vec3(X + 1, Y + 1, Z + 1), fx - 1.0f, fy - 1.0f, fz - 1.0f),
                    u,
                    v,
                    w);
}

ccl_device_inline float mx_cell_noise_float(const float2 p)
{
  return uint_to_float_incl(hash_uint2(uint(floor_to_int(p.x)), uint(floor_to_int(p.y))));
}

ccl_device_inline float mx_cell_noise_float(const float3 p)
{
  return uint_to_float_incl(
      hash_uint3(uint(floor_to_int(p.x)), uint(floor_to_int(p.y)), uint(floor_to_int(p.z))));
}

ccl_device_inline float3 mx_cell_noise_vec3(const float2 p)
{
  const int ix = floor_to_int(p.x);
  const int iy = floor_to_int(p.y);
  return make_float3(uint_to_float_incl(hash_uint3(uint(ix), uint(iy), 0u)),
                     uint_to_float_incl(hash_uint3(uint(ix), uint(iy), 1u)),
                     uint_to_float_incl(hash_uint3(uint(ix), uint(iy), 2u)));
}

ccl_device_inline float3 mx_cell_noise_vec3(const float3 p)
{
  uint a, b, c;
  a = b = c = 0xdeadbeefu + (4u << 2u) + 13u;
  a += uint(floor_to_int(p.x));
  b += uint(floor_to_int(p.y));
  c += uint(floor_to_int(p.z));
  mx_bjmix(&a, &b, &c);
  return make_float3(uint_to_float_incl(mx_bjfinal(a, b, c)),
                     uint_to_float_incl(mx_bjfinal(a + 1u, b, c)),
                     uint_to_float_incl(mx_bjfinal(a + 2u, b, c)));
}

ccl_device float3 mx_fractal_noise_vec3(float2 p,
                                        const int octaves,
                                        const float lacunarity,
                                        const float diminish)
{
  float3 result = zero_float3();
  float amplitude = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    result += amplitude * mx_perlin_noise_vec3(p);
    amplitude *= diminish;
    p *= lacunarity;
  }
  return result;
}

ccl_device float3 mx_fractal_noise_vec3(float3 p,
                                        const int octaves,
                                        const float lacunarity,
                                        const float diminish)
{
  float3 result = zero_float3();
  float amplitude = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    result += amplitude * mx_perlin_noise_vec3(p);
    amplitude *= diminish;
    p *= lacunarity;
  }
  return result;
}

ccl_device_inline float2 mx_worley_cell_position(const int x,
                                                 const int y,
                                                 const int xoff,
                                                 const int yoff,
                                                 const float jitter)
{
  float3 tmp = mx_cell_noise_vec3(make_float2(float(x + xoff), float(y + yoff)));
  float2 off = make_float2(tmp.x, tmp.y);
  off = (off - make_float2(0.5f, 0.5f)) * jitter + make_float2(0.5f, 0.5f);
  return make_float2(float(x), float(y)) + off;
}

ccl_device_inline float3 mx_worley_cell_position(const int x,
                                                 const int y,
                                                 const int z,
                                                 const int xoff,
                                                 const int yoff,
                                                 const int zoff,
                                                 const float jitter)
{
  float3 off = mx_cell_noise_vec3(
      make_float3(float(x + xoff), float(y + yoff), float(z + zoff)));
  off = (off - make_float3(0.5f, 0.5f, 0.5f)) * jitter + make_float3(0.5f, 0.5f, 0.5f);
  return make_float3(float(x), float(y), float(z)) + off;
}

ccl_device_inline float mx_worley_distance(const float2 p,
                                           const int x,
                                           const int y,
                                           const int xoff,
                                           const int yoff,
                                           const float jitter,
                                           const int metric)
{
  const float2 diff = mx_worley_cell_position(x, y, xoff, yoff, jitter) - p;
  if (metric == 2) {
    return fabsf(diff.x) + fabsf(diff.y);
  }
  if (metric == 3) {
    return fmaxf(fabsf(diff.x), fabsf(diff.y));
  }
  return dot(diff, diff);
}

ccl_device_inline float mx_worley_distance(const float3 p,
                                           const int x,
                                           const int y,
                                           const int z,
                                           const int xoff,
                                           const int yoff,
                                           const int zoff,
                                           const float jitter,
                                           const int metric)
{
  const float3 diff = mx_worley_cell_position(x, y, z, xoff, yoff, zoff, jitter) - p;
  if (metric == 2) {
    return fabsf(diff.x) + fabsf(diff.y) + fabsf(diff.z);
  }
  if (metric == 3) {
    return fmaxf(fmaxf(fabsf(diff.x), fabsf(diff.y)), fabsf(diff.z));
  }
  return dot(diff, diff);
}

ccl_device float mx_worley_noise_float(const float2 p,
                                       const float jitter,
                                       const int style,
                                       const int metric)
{
  int X, Y;
  const float2 localpos = make_float2(mx_floorfrac(p.x, &X), mx_floorfrac(p.y, &Y));
  float sqdist = 1e6f;
  float2 minpos = zero_float2();
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      const float dist = mx_worley_distance(localpos, x, y, X, Y, jitter, metric);
      const float2 cellpos = mx_worley_cell_position(x, y, X, Y, jitter) - localpos;
      if (dist < sqdist) {
        sqdist = dist;
        minpos = cellpos;
      }
    }
  }
  if (style == 1) {
    return mx_cell_noise_float(minpos + p);
  }
  return metric == 0 ? sqrtf(sqdist) : sqdist;
}

ccl_device float mx_worley_noise_float(const float3 p,
                                       const float jitter,
                                       const int style,
                                       const int metric)
{
  int X, Y, Z;
  const float3 localpos = make_float3(
      mx_floorfrac(p.x, &X), mx_floorfrac(p.y, &Y), mx_floorfrac(p.z, &Z));
  float sqdist = 1e6f;
  float3 minpos = zero_float3();
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      for (int z = -1; z <= 1; ++z) {
        const float dist = mx_worley_distance(localpos, x, y, z, X, Y, Z, jitter, metric);
        const float3 cellpos = mx_worley_cell_position(x, y, z, X, Y, Z, jitter) - localpos;
        if (dist < sqdist) {
          sqdist = dist;
          minpos = cellpos;
        }
      }
    }
  }
  if (style == 1) {
    return mx_cell_noise_float(minpos + p);
  }
  return metric == 0 ? sqrtf(sqdist) : sqdist;
}

ccl_device float3 mx_worley_noise_vec3(const float2 p,
                                       const float jitter,
                                       const int style,
                                       const int metric)
{
  int X, Y;
  const float2 localpos = make_float2(mx_floorfrac(p.x, &X), mx_floorfrac(p.y, &Y));
  float3 sqdist = make_float3(1e6f, 1e6f, 1e6f);
  float2 minpos = zero_float2();
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      const float dist = mx_worley_distance(localpos, x, y, X, Y, jitter, metric);
      const float2 cellpos = mx_worley_cell_position(x, y, X, Y, jitter) - localpos;
      if (dist < sqdist.x) {
        sqdist.z = sqdist.y;
        sqdist.y = sqdist.x;
        sqdist.x = dist;
        minpos = cellpos;
      }
      else if (dist < sqdist.y) {
        sqdist.z = sqdist.y;
        sqdist.y = dist;
      }
      else if (dist < sqdist.z) {
        sqdist.z = dist;
      }
    }
  }
  if (style == 1) {
    return mx_cell_noise_vec3(minpos + p);
  }
  return metric == 0 ? make_float3(sqrtf(sqdist.x), sqrtf(sqdist.y), sqrtf(sqdist.z)) : sqdist;
}

ccl_device float3 mx_worley_noise_vec3(const float3 p,
                                       const float jitter,
                                       const int style,
                                       const int metric)
{
  int X, Y, Z;
  const float3 localpos = make_float3(
      mx_floorfrac(p.x, &X), mx_floorfrac(p.y, &Y), mx_floorfrac(p.z, &Z));
  float3 sqdist = make_float3(1e6f, 1e6f, 1e6f);
  float3 minpos = zero_float3();
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      for (int z = -1; z <= 1; ++z) {
        const float dist = mx_worley_distance(localpos, x, y, z, X, Y, Z, jitter, metric);
        const float3 cellpos = mx_worley_cell_position(x, y, z, X, Y, Z, jitter) - localpos;
        if (dist < sqdist.x) {
          sqdist.z = sqdist.y;
          sqdist.y = sqdist.x;
          sqdist.x = dist;
          minpos = cellpos;
        }
        else if (dist < sqdist.y) {
          sqdist.z = sqdist.y;
          sqdist.y = dist;
        }
        else if (dist < sqdist.z) {
          sqdist.z = dist;
        }
      }
    }
  }
  if (style == 1) {
    return mx_cell_noise_vec3(minpos + p);
  }
  return metric == 0 ? make_float3(sqrtf(sqdist.x), sqrtf(sqdist.y), sqrtf(sqdist.z)) : sqdist;
}

ccl_device_inline float2 mx_rotate2d(const float2 in, const float amount)
{
  const float radians = amount * M_PI_F / 180.0f;
  const float sa = sinf(radians);
  const float ca = cosf(radians);
  return make_float2(ca * in.x + sa * in.y, -sa * in.x + ca * in.y);
}

ccl_device_inline float3 mx_rotate3d(const float3 in, const float amount)
{
  const float3 axis = normalize(make_float3(0.1f, 1.0f, 0.0f));
  const float radians = amount * M_PI_F / 180.0f;
  const float s = sinf(radians);
  const float c = cosf(radians);
  return in * c + cross(axis, in) * s + axis * dot(axis, in) * (1.0f - c);
}

ccl_device_inline float mx_range_float(const float value,
                                       const float out_min,
                                       const float out_max,
                                       const bool clamp_output)
{
  const float result = out_min + value * (out_max - out_min);
  return clamp_output ? clamp(result, fminf(out_min, out_max), fmaxf(out_min, out_max)) : result;
}

ccl_device_noinline void svm_node_tex_mx_noise(ccl_private float *ccl_restrict stack,
                                               const ccl_global SVMNodeTexMxNoise &ccl_restrict node)
{
  const float3 vector = stack_load(stack, node.vector);
  const float amplitude = stack_load(stack, node.amplitude);
  const float pivot = stack_load(stack, node.pivot);
  const int octaves = clamp(float_to_int(stack_load(stack, node.octaves)), 0, 32);
  const float lacunarity = stack_load(stack, node.lacunarity);
  const float diminish = stack_load(stack, node.diminish);
  const float jitter = stack_load(stack, node.jitter);
  const int style = float_to_int(stack_load(stack, node.style));
  const float3 freq = stack_load(stack, node.freq);
  const float3 offset = stack_load(stack, node.offset);
  const float out_min = stack_load(stack, node.out_min);
  const float out_max = stack_load(stack, node.out_max);
  const bool clamp_output = stack_load(stack, node.clamp_output) != 0.0f;

  const bool is_2d = node.dimensions == 2;
  const float2 p2 = make_float2(vector.x, vector.y);
  const float3 p3 = vector;

  float value = 0.0f;
  float3 color = zero_float3();
  int noise_type = int(node.noise_type);

  if (noise_type == NODE_MX_NOISE_UNIFIED) {
    const int unified_type = float_to_int(stack_load(stack, node.unified_type));
    const float cell_jitter = (jitter - 1.0f) * 90000.0f;
    if (is_2d) {
      const float2 apply_offset = make_float2(vector.x * freq.x, vector.y * freq.y) +
                                  make_float2(offset.x, offset.y);
      const float2 apply_cell_jitter = mx_rotate2d(apply_offset, cell_jitter);
      if (unified_type == 1) {
        value = mx_cell_noise_float(apply_cell_jitter);
        color = mx_cell_noise_vec3(apply_cell_jitter);
      }
      else if (unified_type == 2) {
        value = mx_worley_noise_float(apply_offset, jitter, style, 0);
        color = mx_worley_noise_vec3(apply_offset, jitter, style, 0);
      }
      else if (unified_type == 3) {
        const float3 fractal_position = make_float3(apply_offset.x, apply_offset.y, cell_jitter);
        /* MaterialX mx_fractal_noise_float, using Blender's equivalent wrapped fBM. */
        value = octaves > 0 ?
                    noise_fbm(fractal_position, float(octaves - 1), diminish, lacunarity, false) :
                    0.0f;
        color = mx_fractal_noise_vec3(fractal_position, octaves, lacunarity, diminish);
      }
      else {
        /* MaterialX mx_perlin_noise_float, using Blender's equivalent wrapped noise. */
        value = snoise_2d(apply_cell_jitter) * 0.5f + 0.5f;
        color = mx_perlin_noise_vec3(apply_cell_jitter) * 0.5f + make_float3(0.5f, 0.5f, 0.5f);
      }
    }
    else {
      const float3 apply_offset = vector * freq + offset;
      const float3 apply_cell_jitter = mx_rotate3d(apply_offset, cell_jitter);
      if (unified_type == 1) {
        value = mx_cell_noise_float(apply_cell_jitter);
        color = mx_cell_noise_vec3(apply_cell_jitter);
      }
      else if (unified_type == 2) {
        value = mx_worley_noise_float(apply_offset, jitter, style, 0);
        color = mx_worley_noise_vec3(apply_offset, jitter, style, 0);
      }
      else if (unified_type == 3) {
        /* MaterialX mx_fractal_noise_float, using Blender's equivalent wrapped fBM. */
        value = octaves > 0 ?
                    noise_fbm(apply_cell_jitter, float(octaves - 1), diminish, lacunarity, false) :
                    0.0f;
        color = mx_fractal_noise_vec3(apply_cell_jitter, octaves, lacunarity, diminish);
      }
      else {
        /* MaterialX mx_perlin_noise_float, using Blender's equivalent wrapped noise. */
        value = snoise_3d(apply_cell_jitter) * 0.5f + 0.5f;
        color = mx_perlin_noise_vec3(apply_cell_jitter) * 0.5f + make_float3(0.5f, 0.5f, 0.5f);
      }
    }
    value = mx_range_float(value, out_min, out_max, clamp_output);
    color = make_float3(mx_range_float(color.x, out_min, out_max, clamp_output),
                        mx_range_float(color.y, out_min, out_max, clamp_output),
                        mx_range_float(color.z, out_min, out_max, clamp_output));
  }
  else if (noise_type == NODE_MX_NOISE_CELL) {
    value = is_2d ? mx_cell_noise_float(p2) : mx_cell_noise_float(p3);
    color = is_2d ? mx_cell_noise_vec3(p2) : mx_cell_noise_vec3(p3);
  }
  else if (noise_type == NODE_MX_NOISE_WORLEY) {
    value = is_2d ? mx_worley_noise_float(p2, jitter, style, 0) :
                    mx_worley_noise_float(p3, jitter, style, 0);
    color = is_2d ? mx_worley_noise_vec3(p2, jitter, style, 0) :
                    mx_worley_noise_vec3(p3, jitter, style, 0);
  }
  else if (noise_type == NODE_MX_NOISE_FRACTAL) {
    /* MaterialX mx_fractal_noise_float, using Blender's equivalent wrapped fBM. */
    value = octaves <= 0 ? 0.0f :
                           (is_2d ? noise_fbm(p2, float(octaves - 1), diminish, lacunarity, false) :
                                    noise_fbm(p3, float(octaves - 1), diminish, lacunarity, false));
    color = is_2d ? mx_fractal_noise_vec3(p2, octaves, lacunarity, diminish) :
                    mx_fractal_noise_vec3(p3, octaves, lacunarity, diminish);
    value *= amplitude;
    color *= amplitude;
  }
  else {
    /* MaterialX mx_perlin_noise_float, using Blender's equivalent wrapped noise. */
    value = is_2d ? snoise_2d(p2) : snoise_3d(p3);
    color = is_2d ? mx_perlin_noise_vec3(p2) : mx_perlin_noise_vec3(p3);
    value = value * amplitude + pivot;
    color = color * amplitude + make_float3(pivot, pivot, pivot);
  }

  if (stack_valid(node.value_offset)) {
    stack_store_float(stack, node.value_offset, value);
  }
  if (stack_valid(node.color_offset)) {
    stack_store_float3(stack, node.color_offset, color);
  }
  if (stack_valid(node.vector_offset)) {
    stack_store_float3(stack, node.vector_offset, color);
  }
}

CCL_NAMESPACE_END
