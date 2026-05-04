/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "BLI_noise.hh"

#include "FN_multi_function.hh"

#include "NOD_multi_function.hh"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace blender::nodes::node_shader_tex_mx_noise_cc {

enum MxNoiseType {
  MX_NOISE_PERLIN = 0,
  MX_NOISE_FRACTAL = 1,
  MX_NOISE_CELL = 2,
  MX_NOISE_WORLEY = 3,
  MX_NOISE_UNIFIED = 4,
};

/* MaterialX-compatible noise math, adapted from MaterialX's GLSL noise library,
 * which is derived from Open Shading Language's noise implementation. */

struct MxNoiseNodeInfo {
  int dimensions;
  int noise_type;
};

static MxNoiseNodeInfo node_info(const int legacy_type)
{
  switch (legacy_type) {
    case SH_NODE_TEX_MX_NOISE2D:
      return {2, MX_NOISE_PERLIN};
    case SH_NODE_TEX_MX_NOISE3D:
      return {3, MX_NOISE_PERLIN};
    case SH_NODE_TEX_MX_FRACTAL2D:
      return {2, MX_NOISE_FRACTAL};
    case SH_NODE_TEX_MX_FRACTAL3D:
      return {3, MX_NOISE_FRACTAL};
    case SH_NODE_TEX_MX_CELLNOISE2D:
      return {2, MX_NOISE_CELL};
    case SH_NODE_TEX_MX_CELLNOISE3D:
      return {3, MX_NOISE_CELL};
    case SH_NODE_TEX_MX_WORLEYNOISE2D:
      return {2, MX_NOISE_WORLEY};
    case SH_NODE_TEX_MX_WORLEYNOISE3D:
      return {3, MX_NOISE_WORLEY};
    case SH_NODE_TEX_MX_UNIFIEDNOISE2D:
      return {2, MX_NOISE_UNIFIED};
    case SH_NODE_TEX_MX_UNIFIEDNOISE3D:
      return {3, MX_NOISE_UNIFIED};
  }
  BLI_assert_unreachable();
  return {3, MX_NOISE_PERLIN};
}

static float mx_select(const bool b, const float t, const float f)
{
  return b ? t : f;
}

static float mx_negate_if(const float val, const bool b)
{
  return b ? -val : val;
}

static float mx_floorfrac(const float x, int &i)
{
  i = int(floorf(x));
  return x - float(i);
}

static float mx_bilerp(const float v0,
                       const float v1,
                       const float v2,
                       const float v3,
                       const float s,
                       const float t)
{
  const float s1 = 1.0f - s;
  return (1.0f - t) * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s);
}

static float3 mx_bilerp(const float3 v0,
                        const float3 v1,
                        const float3 v2,
                        const float3 v3,
                        const float s,
                        const float t)
{
  const float s1 = 1.0f - s;
  return (1.0f - t) * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s);
}

static float mx_trilerp(const float v0,
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

static float3 mx_trilerp(const float3 v0,
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

static float mx_gradient_float(const uint32_t hash, const float x, const float y)
{
  const uint32_t h = hash & 7u;
  const float u = mx_select(h < 4u, x, y);
  const float v = 2.0f * mx_select(h < 4u, y, x);
  return mx_negate_if(u, bool(h & 1u)) + mx_negate_if(v, bool(h & 2u));
}

static float mx_gradient_float(const uint32_t hash, const float x, const float y, const float z)
{
  const uint32_t h = hash & 15u;
  const float u = mx_select(h < 8u, x, y);
  const float v = mx_select(h < 4u, y, mx_select((h == 12u) || (h == 14u), x, z));
  return mx_negate_if(u, bool(h & 1u)) + mx_negate_if(v, bool(h & 2u));
}

static uint32_t mx_rotl32(const uint32_t x, const int k)
{
  return (x << k) | (x >> (32 - k));
}

static void mx_bjmix(uint32_t &a, uint32_t &b, uint32_t &c)
{
  a -= c; a ^= mx_rotl32(c, 4); c += b;
  b -= a; b ^= mx_rotl32(a, 6); a += c;
  c -= b; c ^= mx_rotl32(b, 8); b += a;
  a -= c; a ^= mx_rotl32(c, 16); c += b;
  b -= a; b ^= mx_rotl32(a, 19); a += c;
  c -= b; c ^= mx_rotl32(b, 4); b += a;
}

static uint32_t mx_bjfinal(uint32_t a, uint32_t b, uint32_t c)
{
  c ^= b; c -= mx_rotl32(b, 14);
  a ^= c; a -= mx_rotl32(c, 11);
  b ^= a; b -= mx_rotl32(a, 25);
  c ^= b; c -= mx_rotl32(b, 16);
  a ^= c; a -= mx_rotl32(c, 4);
  b ^= a; b -= mx_rotl32(a, 14);
  c ^= b; c -= mx_rotl32(b, 24);
  return c;
}

static float mx_fade(const float t)
{
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float3 mx_hash_vec3(const int x, const int y)
{
  const uint32_t h = noise::hash(uint32_t(x), uint32_t(y));
  return float3(float(h & 0xffu), float((h >> 8) & 0xffu), float((h >> 16) & 0xffu));
}

static float3 mx_hash_vec3(const int x, const int y, const int z)
{
  const uint32_t h = noise::hash(uint32_t(x), uint32_t(y), uint32_t(z));
  return float3(float(h & 0xffu), float((h >> 8) & 0xffu), float((h >> 16) & 0xffu));
}

static float3 mx_gradient_vec3(const float3 hash, const float x, const float y)
{
  return float3(mx_gradient_float(uint32_t(hash.x), x, y),
                mx_gradient_float(uint32_t(hash.y), x, y),
                mx_gradient_float(uint32_t(hash.z), x, y));
}

static float3 mx_gradient_vec3(const float3 hash, const float x, const float y, const float z)
{
  return float3(mx_gradient_float(uint32_t(hash.x), x, y, z),
                mx_gradient_float(uint32_t(hash.y), x, y, z),
                mx_gradient_float(uint32_t(hash.z), x, y, z));
}

static float mx_dot(const float2 a, const float2 b)
{
  return a.x * b.x + a.y * b.y;
}

static float mx_dot(const float3 a, const float3 b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float3 mx_cross(const float3 a, const float3 b)
{
  return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static float3 mx_normalize(const float3 v)
{
  const float len = sqrtf(mx_dot(v, v));
  return len > 0.0f ? v / len : float3(0.0f);
}

static float3 mx_perlin_noise_vec3(const float2 p)
{
  int X, Y;
  const float fx = mx_floorfrac(p.x, X);
  const float fy = mx_floorfrac(p.y, Y);
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

static float3 mx_perlin_noise_vec3(const float3 p)
{
  int X, Y, Z;
  const float fx = mx_floorfrac(p.x, X);
  const float fy = mx_floorfrac(p.y, Y);
  const float fz = mx_floorfrac(p.z, Z);
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

static float mx_cell_noise_float(const float2 p)
{
  return noise::hash_to_float(uint32_t(int(floorf(p.x))), uint32_t(int(floorf(p.y))));
}

static float mx_cell_noise_float(const float3 p)
{
  return noise::hash_to_float(
      uint32_t(int(floorf(p.x))), uint32_t(int(floorf(p.y))), uint32_t(int(floorf(p.z))));
}

static float3 mx_cell_noise_vec3(const float2 p)
{
  const int ix = int(floorf(p.x));
  const int iy = int(floorf(p.y));
  return float3(noise::hash_to_float(uint32_t(ix), uint32_t(iy), 0u),
                noise::hash_to_float(uint32_t(ix), uint32_t(iy), 1u),
                noise::hash_to_float(uint32_t(ix), uint32_t(iy), 2u));
}

static float3 mx_cell_noise_vec3(const float3 p)
{
  uint32_t a, b, c;
  a = b = c = 0xdeadbeefu + (4u << 2u) + 13u;
  a += uint32_t(int(floorf(p.x)));
  b += uint32_t(int(floorf(p.y)));
  c += uint32_t(int(floorf(p.z)));
  mx_bjmix(a, b, c);
  return float3(float(mx_bjfinal(a, b, c)) / float(0xffffffffu),
                float(mx_bjfinal(a + 1u, b, c)) / float(0xffffffffu),
                float(mx_bjfinal(a + 2u, b, c)) / float(0xffffffffu));
}

static float3 mx_fractal_noise_vec3(float2 p,
                                    const int octaves,
                                    const float lacunarity,
                                    const float diminish)
{
  float3 result(0.0f);
  float amplitude = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    result += amplitude * mx_perlin_noise_vec3(p);
    amplitude *= diminish;
    p *= lacunarity;
  }
  return result;
}

static float3 mx_fractal_noise_vec3(float3 p,
                                    const int octaves,
                                    const float lacunarity,
                                    const float diminish)
{
  float3 result(0.0f);
  float amplitude = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    result += amplitude * mx_perlin_noise_vec3(p);
    amplitude *= diminish;
    p *= lacunarity;
  }
  return result;
}

static float2 mx_worley_cell_position(const int x,
                                      const int y,
                                      const int xoff,
                                      const int yoff,
                                      const float jitter)
{
  float3 tmp = mx_cell_noise_vec3(float2(float(x + xoff), float(y + yoff)));
  float2 off = float2(tmp.x, tmp.y);
  off = (off - float2(0.5f)) * jitter + float2(0.5f);
  return float2(float(x), float(y)) + off;
}

static float3 mx_worley_cell_position(const int x,
                                      const int y,
                                      const int z,
                                      const int xoff,
                                      const int yoff,
                                      const int zoff,
                                      const float jitter)
{
  float3 off = mx_cell_noise_vec3(float3(float(x + xoff), float(y + yoff), float(z + zoff)));
  off = (off - float3(0.5f)) * jitter + float3(0.5f);
  return float3(float(x), float(y), float(z)) + off;
}

static float mx_worley_distance(const float2 p,
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
    return std::max(fabsf(diff.x), fabsf(diff.y));
  }
  return mx_dot(diff, diff);
}

static float mx_worley_distance(const float3 p,
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
    return std::max(std::max(fabsf(diff.x), fabsf(diff.y)), fabsf(diff.z));
  }
  return mx_dot(diff, diff);
}

static float mx_worley_noise_float(const float2 p,
                                   const float jitter,
                                   const int style,
                                   const int metric)
{
  int X, Y;
  const float2 localpos = float2(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y));
  float sqdist = 1e6f;
  float2 minpos(0.0f);
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

static float mx_worley_noise_float(const float3 p,
                                   const float jitter,
                                   const int style,
                                   const int metric)
{
  int X, Y, Z;
  const float3 localpos = float3(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y), mx_floorfrac(p.z, Z));
  float sqdist = 1e6f;
  float3 minpos(0.0f);
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

static float3 mx_worley_noise_vec3(const float2 p,
                                   const float jitter,
                                   const int style,
                                   const int metric)
{
  int X, Y;
  const float2 localpos = float2(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y));
  float3 sqdist(1e6f);
  float2 minpos(0.0f);
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
  return metric == 0 ? float3(sqrtf(sqdist.x), sqrtf(sqdist.y), sqrtf(sqdist.z)) : sqdist;
}

static float3 mx_worley_noise_vec3(const float3 p,
                                   const float jitter,
                                   const int style,
                                   const int metric)
{
  int X, Y, Z;
  const float3 localpos = float3(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y), mx_floorfrac(p.z, Z));
  float3 sqdist(1e6f);
  float3 minpos(0.0f);
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
  return metric == 0 ? float3(sqrtf(sqdist.x), sqrtf(sqdist.y), sqrtf(sqdist.z)) : sqdist;
}

static float2 mx_rotate2d(const float2 in, const float amount)
{
  constexpr float pi = 3.14159265358979323846f;
  const float radians = amount * pi / 180.0f;
  const float sa = sinf(radians);
  const float ca = cosf(radians);
  return float2(ca * in.x + sa * in.y, -sa * in.x + ca * in.y);
}

static float3 mx_rotate3d(const float3 in, const float amount)
{
  constexpr float pi = 3.14159265358979323846f;
  const float3 axis = mx_normalize(float3(0.1f, 1.0f, 0.0f));
  const float radians = amount * pi / 180.0f;
  const float s = sinf(radians);
  const float c = cosf(radians);
  return in * c + mx_cross(axis, in) * s + axis * mx_dot(axis, in) * (1.0f - c);
}

static float mx_range_float(const float value,
                            const float out_min,
                            const float out_max,
                            const bool clamp_output)
{
  const float result = out_min + value * (out_max - out_min);
  return clamp_output ? std::clamp(result, std::min(out_min, out_max), std::max(out_min, out_max)) :
                        result;
}

struct MxNoiseResult {
  float value;
  float3 color;
};

static MxNoiseResult mx_noise_eval(const int dimensions,
                                   const int noise_type,
                                   const float3 vector,
                                   const float amplitude,
                                   const float pivot,
                                   const int octaves,
                                   const float lacunarity,
                                   const float diminish,
                                   const float jitter,
                                   const int style,
                                   const int unified_type,
                                   const float3 freq,
                                   const float3 offset,
                                   const float out_min,
                                   const float out_max,
                                   const bool clamp_output)
{
  const bool is_2d = dimensions == 2;
  const float2 p2(vector.x, vector.y);
  const float3 p3 = vector;

  float value = 0.0f;
  float3 color(0.0f);

  if (noise_type == MX_NOISE_UNIFIED) {
    const float cell_jitter = (jitter - 1.0f) * 90000.0f;
    if (is_2d) {
      const float2 apply_offset = float2(vector.x * freq.x, vector.y * freq.y) +
                                  float2(offset.x, offset.y);
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
        const float3 fractal_position(apply_offset.x, apply_offset.y, cell_jitter);
        /* MaterialX mx_fractal_noise_float, using Blender's equivalent wrapped fBM. */
        value = octaves > 0 ?
                    noise::perlin_fbm(
                        fractal_position, float(octaves - 1), diminish, lacunarity, false) :
                    0.0f;
        color = mx_fractal_noise_vec3(fractal_position, octaves, lacunarity, diminish);
      }
      else {
        /* MaterialX mx_perlin_noise_float, using Blender's equivalent wrapped noise. */
        value = noise::perlin_signed(apply_cell_jitter) * 0.5f + 0.5f;
        color = mx_perlin_noise_vec3(apply_cell_jitter) * 0.5f + float3(0.5f);
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
                    noise::perlin_fbm(
                        apply_cell_jitter, float(octaves - 1), diminish, lacunarity, false) :
                    0.0f;
        color = mx_fractal_noise_vec3(apply_cell_jitter, octaves, lacunarity, diminish);
      }
      else {
        /* MaterialX mx_perlin_noise_float, using Blender's equivalent wrapped noise. */
        value = noise::perlin_signed(apply_cell_jitter) * 0.5f + 0.5f;
        color = mx_perlin_noise_vec3(apply_cell_jitter) * 0.5f + float3(0.5f);
      }
    }
    value = mx_range_float(value, out_min, out_max, clamp_output);
    color = float3(mx_range_float(color.x, out_min, out_max, clamp_output),
                   mx_range_float(color.y, out_min, out_max, clamp_output),
                   mx_range_float(color.z, out_min, out_max, clamp_output));
  }
  else if (noise_type == MX_NOISE_CELL) {
    value = is_2d ? mx_cell_noise_float(p2) : mx_cell_noise_float(p3);
    color = is_2d ? mx_cell_noise_vec3(p2) : mx_cell_noise_vec3(p3);
  }
  else if (noise_type == MX_NOISE_WORLEY) {
    value = is_2d ? mx_worley_noise_float(p2, jitter, style, 0) :
                    mx_worley_noise_float(p3, jitter, style, 0);
    color = is_2d ? mx_worley_noise_vec3(p2, jitter, style, 0) :
                    mx_worley_noise_vec3(p3, jitter, style, 0);
  }
  else if (noise_type == MX_NOISE_FRACTAL) {
    /* MaterialX mx_fractal_noise_float, using Blender's equivalent wrapped fBM. */
    value = octaves <= 0 ?
                0.0f :
                (is_2d ? noise::perlin_fbm(p2, float(octaves - 1), diminish, lacunarity, false) :
                         noise::perlin_fbm(p3, float(octaves - 1), diminish, lacunarity, false));
    color = is_2d ? mx_fractal_noise_vec3(p2, octaves, lacunarity, diminish) :
                    mx_fractal_noise_vec3(p3, octaves, lacunarity, diminish);
    value *= amplitude;
    color *= amplitude;
  }
  else {
    /* MaterialX mx_perlin_noise_float, using Blender's equivalent wrapped noise. */
    value = is_2d ? noise::perlin_signed(p2) : noise::perlin_signed(p3);
    color = is_2d ? mx_perlin_noise_vec3(p2) : mx_perlin_noise_vec3(p3);
    value = value * amplitude + pivot;
    color = color * amplitude + float3(pivot);
  }

  return {value, color};
}

static void declare_vector_input(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Vector"_ustr).implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD);
}

static void declare_common_output(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Value"_ustr).no_muted_links();
}

static void declare_color_output(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Color"_ustr).no_muted_links();
}

static void declare_vector_output(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Vector"_ustr).no_muted_links();
}

static void declare_base_noise(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  declare_vector_input(b);
  b.add_input<decl::Float>("Amplitude"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Pivot"_ustr).default_value(0.0f);
  declare_common_output(b);
  declare_color_output(b);
  declare_vector_output(b);
}

static void declare_fractal_noise(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  declare_vector_input(b);
  b.add_input<decl::Float>("Amplitude"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Octaves"_ustr).default_value(3.0f).min(0.0f).max(32.0f);
  b.add_input<decl::Float>("Lacunarity"_ustr).default_value(2.0f);
  b.add_input<decl::Float>("Diminish"_ustr).default_value(0.5f);
  declare_common_output(b);
  declare_color_output(b);
  declare_vector_output(b);
}

static void declare_cell_noise(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  declare_vector_input(b);
  declare_common_output(b);
}

static void declare_worley_noise(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  declare_vector_input(b);
  b.add_input<decl::Float>("Jitter"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Style"_ustr).default_value(0.0f).min(0.0f).max(1.0f);
  declare_common_output(b);
  declare_vector_output(b);
}

static void declare_unified_noise(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  declare_vector_input(b);
  b.add_input<decl::Vector>("Frequency"_ustr).default_value({1.0f, 1.0f, 1.0f});
  b.add_input<decl::Vector>("Offset"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<decl::Float>("Type"_ustr).default_value(0.0f).min(0.0f).max(3.0f);
  b.add_input<decl::Float>("Jitter"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Style"_ustr).default_value(0.0f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>("Octaves"_ustr).default_value(3.0f).min(0.0f).max(32.0f);
  b.add_input<decl::Float>("Lacunarity"_ustr).default_value(2.0f);
  b.add_input<decl::Float>("Diminish"_ustr).default_value(0.5f);
  b.add_input<decl::Float>("Out Min"_ustr).default_value(0.0f);
  b.add_input<decl::Float>("Out Max"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Clamp Output"_ustr).default_value(1.0f).min(0.0f).max(1.0f);
  declare_common_output(b);
}

static void declare_noise_2d(NodeDeclarationBuilder &b)
{
  declare_base_noise(b);
}

static void declare_noise_3d(NodeDeclarationBuilder &b)
{
  declare_base_noise(b);
}

static void declare_fractal_2d(NodeDeclarationBuilder &b)
{
  declare_fractal_noise(b);
}

static void declare_fractal_3d(NodeDeclarationBuilder &b)
{
  declare_fractal_noise(b);
}

static void declare_cell_2d(NodeDeclarationBuilder &b)
{
  declare_cell_noise(b);
}

static void declare_cell_3d(NodeDeclarationBuilder &b)
{
  declare_cell_noise(b);
}

static void declare_worley_2d(NodeDeclarationBuilder &b)
{
  declare_worley_noise(b);
}

static void declare_worley_3d(NodeDeclarationBuilder &b)
{
  declare_worley_noise(b);
}

static void declare_unified_2d(NodeDeclarationBuilder &b)
{
  declare_unified_noise(b);
}

static void declare_unified_3d(NodeDeclarationBuilder &b)
{
  declare_unified_noise(b);
}

static const char *gpu_shader_get_name(const int noise_type)
{
  switch (noise_type) {
    case MX_NOISE_PERLIN:
      return "node_mx_noise";
    case MX_NOISE_FRACTAL:
      return "node_mx_fractal_noise";
    case MX_NOISE_CELL:
      return "node_mx_cell_noise";
    case MX_NOISE_WORLEY:
      return "node_mx_worley_noise";
    case MX_NOISE_UNIFIED:
      return "node_mx_unified_noise";
  }
  BLI_assert_unreachable();
  return "node_mx_noise";
}

static int gpu_shader_mx_noise(GPUMaterial *mat,
                               bNode *node,
                               bNodeExecData * /*execdata*/,
                               GPUNodeStack *in,
                               GPUNodeStack *out)
{
  node_shader_gpu_default_tex_coord(mat, node, &in[0].link);

  const MxNoiseNodeInfo info = node_info(node->type_legacy);
  float dimensions = float(info.dimensions);
  return GPU_stack_link(
      mat, node, gpu_shader_get_name(info.noise_type), in, out, GPU_constant(&dimensions));
}

class MxNoiseFunction : public mf::MultiFunction {
 private:
  int dimensions_;
  int noise_type_;
  mf::Signature signature_;

 public:
  MxNoiseFunction(const int dimensions, const int noise_type)
      : dimensions_(dimensions), noise_type_(noise_type)
  {
    mf::SignatureBuilder builder{"MaterialX Noise", signature_};
    builder.single_input<float3>("Vector");
    switch (noise_type_) {
      case MX_NOISE_PERLIN:
        builder.single_input<float>("Amplitude");
        builder.single_input<float>("Pivot");
        break;
      case MX_NOISE_FRACTAL:
        builder.single_input<float>("Amplitude");
        builder.single_input<float>("Octaves");
        builder.single_input<float>("Lacunarity");
        builder.single_input<float>("Diminish");
        break;
      case MX_NOISE_WORLEY:
        builder.single_input<float>("Jitter");
        builder.single_input<float>("Style");
        break;
      case MX_NOISE_UNIFIED:
        builder.single_input<float3>("Frequency");
        builder.single_input<float3>("Offset");
        builder.single_input<float>("Type");
        builder.single_input<float>("Jitter");
        builder.single_input<float>("Style");
        builder.single_input<float>("Octaves");
        builder.single_input<float>("Lacunarity");
        builder.single_input<float>("Diminish");
        builder.single_input<float>("Out Min");
        builder.single_input<float>("Out Max");
        builder.single_input<float>("Clamp Output");
        break;
      case MX_NOISE_CELL:
        break;
    }
    builder.single_output<float>("Value", mf::ParamFlag::SupportsUnusedOutput);
    if (ELEM(noise_type_, MX_NOISE_PERLIN, MX_NOISE_FRACTAL)) {
      builder.single_output<ColorGeometry4f>("Color", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<float3>("Vector", mf::ParamFlag::SupportsUnusedOutput);
    }
    else if (noise_type_ == MX_NOISE_WORLEY) {
      builder.single_output<float3>("Vector", mf::ParamFlag::SupportsUnusedOutput);
    }
    this->set_signature(&signature_);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    int param = 0;
    const VArray<float3> &vector = params.readonly_single_input<float3>(param++, "Vector");

    std::optional<VArray<float>> amplitude;
    std::optional<VArray<float>> pivot;
    std::optional<VArray<float>> octaves;
    std::optional<VArray<float>> lacunarity;
    std::optional<VArray<float>> diminish;
    std::optional<VArray<float>> jitter;
    std::optional<VArray<float>> style;
    std::optional<VArray<float>> unified_type;
    std::optional<VArray<float3>> freq;
    std::optional<VArray<float3>> offset;
    std::optional<VArray<float>> out_min;
    std::optional<VArray<float>> out_max;
    std::optional<VArray<float>> clamp_output;

    if (noise_type_ == MX_NOISE_PERLIN) {
      amplitude = params.readonly_single_input<float>(param++, "Amplitude");
      pivot = params.readonly_single_input<float>(param++, "Pivot");
    }
    else if (noise_type_ == MX_NOISE_FRACTAL) {
      amplitude = params.readonly_single_input<float>(param++, "Amplitude");
      octaves = params.readonly_single_input<float>(param++, "Octaves");
      lacunarity = params.readonly_single_input<float>(param++, "Lacunarity");
      diminish = params.readonly_single_input<float>(param++, "Diminish");
    }
    else if (noise_type_ == MX_NOISE_WORLEY) {
      jitter = params.readonly_single_input<float>(param++, "Jitter");
      style = params.readonly_single_input<float>(param++, "Style");
    }
    else if (noise_type_ == MX_NOISE_UNIFIED) {
      freq = params.readonly_single_input<float3>(param++, "Frequency");
      offset = params.readonly_single_input<float3>(param++, "Offset");
      unified_type = params.readonly_single_input<float>(param++, "Type");
      jitter = params.readonly_single_input<float>(param++, "Jitter");
      style = params.readonly_single_input<float>(param++, "Style");
      octaves = params.readonly_single_input<float>(param++, "Octaves");
      lacunarity = params.readonly_single_input<float>(param++, "Lacunarity");
      diminish = params.readonly_single_input<float>(param++, "Diminish");
      out_min = params.readonly_single_input<float>(param++, "Out Min");
      out_max = params.readonly_single_input<float>(param++, "Out Max");
      clamp_output = params.readonly_single_input<float>(param++, "Clamp Output");
    }

    MutableSpan<float> r_value = params.uninitialized_single_output_if_required<float>(param++,
                                                                                       "Value");
    MutableSpan<ColorGeometry4f> r_color;
    if (ELEM(noise_type_, MX_NOISE_PERLIN, MX_NOISE_FRACTAL)) {
      r_color = params.uninitialized_single_output_if_required<ColorGeometry4f>(param++, "Color");
    }
    MutableSpan<float3> r_vector;
    if (ELEM(noise_type_, MX_NOISE_PERLIN, MX_NOISE_FRACTAL, MX_NOISE_WORLEY)) {
      r_vector = params.uninitialized_single_output_if_required<float3>(param++, "Vector");
    }

    const bool compute_value = !r_value.is_empty();
    const bool compute_color = !r_color.is_empty();
    const bool compute_vector = !r_vector.is_empty();

    mask.foreach_index([&](const int64_t i) {
      const MxNoiseResult result = mx_noise_eval(
          dimensions_,
          noise_type_,
          vector[i],
          amplitude ? (*amplitude)[i] : 1.0f,
          pivot ? (*pivot)[i] : 0.0f,
          std::clamp(int(octaves ? (*octaves)[i] : 3.0f), 0, 32),
          lacunarity ? (*lacunarity)[i] : 2.0f,
          diminish ? (*diminish)[i] : 0.5f,
          jitter ? (*jitter)[i] : 1.0f,
          int(style ? (*style)[i] : 0.0f),
          int(unified_type ? (*unified_type)[i] : 0.0f),
          freq ? (*freq)[i] : float3(1.0f),
          offset ? (*offset)[i] : float3(0.0f),
          out_min ? (*out_min)[i] : 0.0f,
          out_max ? (*out_max)[i] : 1.0f,
          clamp_output ? (*clamp_output)[i] != 0.0f : true);
      if (compute_value) {
        r_value[i] = result.value;
      }
      if (compute_color) {
        r_color[i] = ColorGeometry4f(result.color.x, result.color.y, result.color.z, 1.0f);
      }
      if (compute_vector) {
        r_vector[i] = result.color;
      }
    });
  }

  void hash_unique(UniqueHashBytes &hash) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(dimensions_);
    hash.add(noise_type_);
  }
};

static void sh_node_mx_noise_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const MxNoiseNodeInfo info = node_info(builder.node().type_legacy);
  builder.construct_and_set_matching_fn<MxNoiseFunction>(info.dimensions, info.noise_type);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  const MxNoiseNodeInfo info = node_info(node_->type_legacy);
  const bool is_2d = info.dimensions == 2;

  NodeItem position = empty();
  if (is_2d) {
    position = get_input_link("Vector", NodeItem::Type::Vector2);
    if (!position) {
      position = texcoord_node();
    }
  }
  else {
    position = get_input_link("Vector", NodeItem::Type::Vector3);
    if (!position) {
      position = create_node("position", NodeItem::Type::Vector3);
    }
  }

  NodeItem::Inputs inputs;
  inputs.emplace_back(is_2d ? "texcoord" : "position", position);

  const char *category = nullptr;
  switch (info.noise_type) {
    case MX_NOISE_PERLIN:
      category = is_2d ? "noise2d" : "noise3d";
      inputs.emplace_back("amplitude", get_input_value("Amplitude", NodeItem::Type::Float));
      inputs.emplace_back("pivot", get_input_value("Pivot", NodeItem::Type::Float));
      break;
    case MX_NOISE_FRACTAL:
      category = is_2d ? "fractal2d" : "fractal3d";
      inputs.emplace_back("amplitude", get_input_value("Amplitude", NodeItem::Type::Float));
      inputs.emplace_back("octaves", get_input_value("Octaves", NodeItem::Type::Integer));
      inputs.emplace_back("lacunarity", get_input_value("Lacunarity", NodeItem::Type::Float));
      inputs.emplace_back("diminish", get_input_value("Diminish", NodeItem::Type::Float));
      break;
    case MX_NOISE_CELL:
      category = is_2d ? "cellnoise2d" : "cellnoise3d";
      break;
    case MX_NOISE_WORLEY:
      category = is_2d ? "worleynoise2d" : "worleynoise3d";
      inputs.emplace_back("jitter", get_input_value("Jitter", NodeItem::Type::Float));
      inputs.emplace_back("style", get_input_value("Style", NodeItem::Type::Integer));
      break;
    case MX_NOISE_UNIFIED:
      category = is_2d ? "unifiednoise2d" : "unifiednoise3d";
      inputs.emplace_back("freq",
                          get_input_value("Frequency", is_2d ? NodeItem::Type::Vector2 :
                                                                NodeItem::Type::Vector3));
      inputs.emplace_back("offset",
                          get_input_value("Offset", is_2d ? NodeItem::Type::Vector2 :
                                                             NodeItem::Type::Vector3));
      inputs.emplace_back("type", get_input_value("Type", NodeItem::Type::Integer));
      inputs.emplace_back("jitter", get_input_value("Jitter", NodeItem::Type::Float));
      inputs.emplace_back("style", get_input_value("Style", NodeItem::Type::Integer));
      inputs.emplace_back("octaves", get_input_value("Octaves", NodeItem::Type::Integer));
      inputs.emplace_back("lacunarity", get_input_value("Lacunarity", NodeItem::Type::Float));
      inputs.emplace_back("diminish", get_input_value("Diminish", NodeItem::Type::Float));
      inputs.emplace_back("outmin", get_input_value("Out Min", NodeItem::Type::Float));
      inputs.emplace_back("outmax", get_input_value("Out Max", NodeItem::Type::Float));
      inputs.emplace_back("clampoutput", get_input_value("Clamp Output", NodeItem::Type::Boolean));
      break;
  }

  NodeItem::Type output_type = NodeItem::Type::Float;
  if (STREQ(socket_out_->identifier, "Color")) {
    output_type = NodeItem::Type::Color3;
  }
  else if (STREQ(socket_out_->identifier, "Vector")) {
    output_type = NodeItem::Type::Vector3;
  }

  return create_node(category, output_type, inputs);
}
#endif
NODE_SHADER_MATERIALX_END

static void register_mx_node_type(bke::bNodeType &ntype,
                                  const UString idname,
                                  const int legacy_type,
                                  const char *ui_name,
                                  bke::NodeDeclareFunction declare)
{
  common_node_type_base(&ntype, idname, legacy_type);
  ntype.ui_name = ui_name;
  ntype.ui_description = "MaterialX-compatible procedural noise";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = declare;
  ntype.gpu_fn = gpu_shader_mx_noise;
  ntype.build_multi_function = sh_node_mx_noise_build_multi_function;
  ntype.materialx_fn = node_shader_materialx;
  blender::bke::node_register_type(ntype);
}

}  // namespace blender::nodes::node_shader_tex_mx_noise_cc

namespace blender {

void register_node_type_sh_tex_mx_noise()
{
  namespace file_ns = nodes::node_shader_tex_mx_noise_cc;

  static bke::bNodeType noise_2d;
  static bke::bNodeType noise_3d;
  static bke::bNodeType fractal_2d;
  static bke::bNodeType fractal_3d;
  static bke::bNodeType cell_2d;
  static bke::bNodeType cell_3d;
  static bke::bNodeType worley_2d;
  static bke::bNodeType worley_3d;
  static bke::bNodeType unified_2d;
  static bke::bNodeType unified_3d;

  file_ns::register_mx_node_type(noise_2d,
                                 "ShaderNodeMxNoise2D"_ustr,
                                 SH_NODE_TEX_MX_NOISE2D,
                                 "MaterialX Noise 2D",
                                 file_ns::declare_noise_2d);
  file_ns::register_mx_node_type(noise_3d,
                                 "ShaderNodeMxNoise3D"_ustr,
                                 SH_NODE_TEX_MX_NOISE3D,
                                 "MaterialX Noise 3D",
                                 file_ns::declare_noise_3d);
  file_ns::register_mx_node_type(fractal_2d,
                                 "ShaderNodeMxFractal2D"_ustr,
                                 SH_NODE_TEX_MX_FRACTAL2D,
                                 "MaterialX Fractal 2D",
                                 file_ns::declare_fractal_2d);
  file_ns::register_mx_node_type(fractal_3d,
                                 "ShaderNodeMxFractal3D"_ustr,
                                 SH_NODE_TEX_MX_FRACTAL3D,
                                 "MaterialX Fractal 3D",
                                 file_ns::declare_fractal_3d);
  file_ns::register_mx_node_type(cell_2d,
                                 "ShaderNodeMxCellNoise2D"_ustr,
                                 SH_NODE_TEX_MX_CELLNOISE2D,
                                 "MaterialX Cell Noise 2D",
                                 file_ns::declare_cell_2d);
  file_ns::register_mx_node_type(cell_3d,
                                 "ShaderNodeMxCellNoise3D"_ustr,
                                 SH_NODE_TEX_MX_CELLNOISE3D,
                                 "MaterialX Cell Noise 3D",
                                 file_ns::declare_cell_3d);
  file_ns::register_mx_node_type(worley_2d,
                                 "ShaderNodeMxWorleyNoise2D"_ustr,
                                 SH_NODE_TEX_MX_WORLEYNOISE2D,
                                 "MaterialX Worley Noise 2D",
                                 file_ns::declare_worley_2d);
  file_ns::register_mx_node_type(worley_3d,
                                 "ShaderNodeMxWorleyNoise3D"_ustr,
                                 SH_NODE_TEX_MX_WORLEYNOISE3D,
                                 "MaterialX Worley Noise 3D",
                                 file_ns::declare_worley_3d);
  file_ns::register_mx_node_type(unified_2d,
                                 "ShaderNodeMxUnifiedNoise2D"_ustr,
                                 SH_NODE_TEX_MX_UNIFIEDNOISE2D,
                                 "MaterialX Unified Noise 2D",
                                 file_ns::declare_unified_2d);
  file_ns::register_mx_node_type(unified_3d,
                                 "ShaderNodeMxUnifiedNoise3D"_ustr,
                                 SH_NODE_TEX_MX_UNIFIEDNOISE3D,
                                 "MaterialX Unified Noise 3D",
                                 file_ns::declare_unified_3d);
}

}  // namespace blender
