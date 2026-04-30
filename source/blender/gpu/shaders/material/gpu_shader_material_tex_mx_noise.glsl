/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* MaterialX-compatible procedural noise.
 *
 * This is adapted from the MaterialX GLSL noise library, which is in turn
 * derived from Open Shading Language's noise implementation.
 */

float mx_select(bool b, float t, float f)
{
  return b ? t : f;
}

float mx_negate_if(float val, bool b)
{
  return b ? -val : val;
}

float mx_floorfrac(float x, out int i)
{
  i = int(floor(x));
  return x - float(i);
}

float mx_bilerp(float v0, float v1, float v2, float v3, float s, float t)
{
  float s1 = 1.0f - s;
  return (1.0f - t) * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s);
}

float3 mx_bilerp(float3 v0, float3 v1, float3 v2, float3 v3, float s, float t)
{
  float s1 = 1.0f - s;
  return (1.0f - t) * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s);
}

float mx_trilerp(
    float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7, float s, float t, float r)
{
  float s1 = 1.0f - s;
  float t1 = 1.0f - t;
  float r1 = 1.0f - r;
  return r1 * (t1 * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s)) +
         r * (t1 * (v4 * s1 + v5 * s) + t * (v6 * s1 + v7 * s));
}

float3 mx_trilerp(float3 v0,
                  float3 v1,
                  float3 v2,
                  float3 v3,
                  float3 v4,
                  float3 v5,
                  float3 v6,
                  float3 v7,
                  float s,
                  float t,
                  float r)
{
  float s1 = 1.0f - s;
  float t1 = 1.0f - t;
  float r1 = 1.0f - r;
  return r1 * (t1 * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s)) +
         r * (t1 * (v4 * s1 + v5 * s) + t * (v6 * s1 + v7 * s));
}

float mx_gradient_float(uint hash, float x, float y)
{
  uint h = hash & 7u;
  float u = mx_select(h < 4u, x, y);
  float v = 2.0f * mx_select(h < 4u, y, x);
  return mx_negate_if(u, bool(h & 1u)) + mx_negate_if(v, bool(h & 2u));
}

float mx_gradient_float(uint hash, float x, float y, float z)
{
  uint h = hash & 15u;
  float u = mx_select(h < 8u, x, y);
  float v = mx_select(h < 4u, y, mx_select((h == 12u) || (h == 14u), x, z));
  return mx_negate_if(u, bool(h & 1u)) + mx_negate_if(v, bool(h & 2u));
}

uint mx_rotl32(uint x, int k)
{
  return (x << k) | (x >> (32 - k));
}

void mx_bjmix(inout uint a, inout uint b, inout uint c)
{
  a -= c; a ^= mx_rotl32(c, 4); c += b;
  b -= a; b ^= mx_rotl32(a, 6); a += c;
  c -= b; c ^= mx_rotl32(b, 8); b += a;
  a -= c; a ^= mx_rotl32(c, 16); c += b;
  b -= a; b ^= mx_rotl32(a, 19); a += c;
  c -= b; c ^= mx_rotl32(b, 4); b += a;
}

uint mx_bjfinal(uint a, uint b, uint c)
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

float mx_bits_to_01(uint bits)
{
  return float(bits) / float(0xffffffffu);
}

float mx_fade(float t)
{
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

uint mx_hash_int(int x)
{
  uint len = 1u;
  uint seed = uint(0xdeadbeef) + (len << 2u) + 13u;
  return mx_bjfinal(seed + uint(x), seed, seed);
}

uint mx_hash_int(int x, int y)
{
  uint len = 2u;
  uint a, b, c;
  a = b = c = uint(0xdeadbeef) + (len << 2u) + 13u;
  a += uint(x);
  b += uint(y);
  return mx_bjfinal(a, b, c);
}

uint mx_hash_int(int x, int y, int z)
{
  uint len = 3u;
  uint a, b, c;
  a = b = c = uint(0xdeadbeef) + (len << 2u) + 13u;
  a += uint(x);
  b += uint(y);
  c += uint(z);
  return mx_bjfinal(a, b, c);
}

float3 mx_hash_vec3(int x, int y)
{
  uint h = mx_hash_int(x, y);
  return float3(float(h & 0xffu), float((h >> 8) & 0xffu), float((h >> 16) & 0xffu));
}

float3 mx_hash_vec3(int x, int y, int z)
{
  uint h = mx_hash_int(x, y, z);
  return float3(float(h & 0xffu), float((h >> 8) & 0xffu), float((h >> 16) & 0xffu));
}

float3 mx_gradient_vec3(float3 hash, float x, float y)
{
  return float3(mx_gradient_float(uint(hash.x), x, y),
                mx_gradient_float(uint(hash.y), x, y),
                mx_gradient_float(uint(hash.z), x, y));
}

float3 mx_gradient_vec3(float3 hash, float x, float y, float z)
{
  return float3(mx_gradient_float(uint(hash.x), x, y, z),
                mx_gradient_float(uint(hash.y), x, y, z),
                mx_gradient_float(uint(hash.z), x, y, z));
}

float mx_perlin_noise_float(float2 p)
{
  int X, Y;
  float fx = mx_floorfrac(p.x, X);
  float fy = mx_floorfrac(p.y, Y);
  float u = mx_fade(fx);
  float v = mx_fade(fy);
  return 0.6616f * mx_bilerp(mx_gradient_float(mx_hash_int(X, Y), fx, fy),
                             mx_gradient_float(mx_hash_int(X + 1, Y), fx - 1.0f, fy),
                             mx_gradient_float(mx_hash_int(X, Y + 1), fx, fy - 1.0f),
                             mx_gradient_float(mx_hash_int(X + 1, Y + 1), fx - 1.0f, fy - 1.0f),
                             u,
                             v);
}

float mx_perlin_noise_float(float3 p)
{
  int X, Y, Z;
  float fx = mx_floorfrac(p.x, X);
  float fy = mx_floorfrac(p.y, Y);
  float fz = mx_floorfrac(p.z, Z);
  float u = mx_fade(fx);
  float v = mx_fade(fy);
  float w = mx_fade(fz);
  return 0.9820f *
         mx_trilerp(mx_gradient_float(mx_hash_int(X, Y, Z), fx, fy, fz),
                    mx_gradient_float(mx_hash_int(X + 1, Y, Z), fx - 1.0f, fy, fz),
                    mx_gradient_float(mx_hash_int(X, Y + 1, Z), fx, fy - 1.0f, fz),
                    mx_gradient_float(mx_hash_int(X + 1, Y + 1, Z), fx - 1.0f, fy - 1.0f, fz),
                    mx_gradient_float(mx_hash_int(X, Y, Z + 1), fx, fy, fz - 1.0f),
                    mx_gradient_float(mx_hash_int(X + 1, Y, Z + 1), fx - 1.0f, fy, fz - 1.0f),
                    mx_gradient_float(mx_hash_int(X, Y + 1, Z + 1), fx, fy - 1.0f, fz - 1.0f),
                    mx_gradient_float(mx_hash_int(X + 1, Y + 1, Z + 1), fx - 1.0f, fy - 1.0f, fz - 1.0f),
                    u,
                    v,
                    w);
}

float3 mx_perlin_noise_vec3(float2 p)
{
  int X, Y;
  float fx = mx_floorfrac(p.x, X);
  float fy = mx_floorfrac(p.y, Y);
  float u = mx_fade(fx);
  float v = mx_fade(fy);
  return 0.6616f *
         mx_bilerp(mx_gradient_vec3(mx_hash_vec3(X, Y), fx, fy),
                   mx_gradient_vec3(mx_hash_vec3(X + 1, Y), fx - 1.0f, fy),
                   mx_gradient_vec3(mx_hash_vec3(X, Y + 1), fx, fy - 1.0f),
                   mx_gradient_vec3(mx_hash_vec3(X + 1, Y + 1), fx - 1.0f, fy - 1.0f),
                   u,
                   v);
}

float3 mx_perlin_noise_vec3(float3 p)
{
  int X, Y, Z;
  float fx = mx_floorfrac(p.x, X);
  float fy = mx_floorfrac(p.y, Y);
  float fz = mx_floorfrac(p.z, Z);
  float u = mx_fade(fx);
  float v = mx_fade(fy);
  float w = mx_fade(fz);
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

float mx_cell_noise_float(float2 p)
{
  return mx_bits_to_01(mx_hash_int(int(floor(p.x)), int(floor(p.y))));
}

float mx_cell_noise_float(float3 p)
{
  return mx_bits_to_01(mx_hash_int(int(floor(p.x)), int(floor(p.y)), int(floor(p.z))));
}

float3 mx_cell_noise_vec3(float2 p)
{
  int ix = int(floor(p.x));
  int iy = int(floor(p.y));
  return float3(mx_bits_to_01(mx_hash_int(ix, iy, 0)),
                mx_bits_to_01(mx_hash_int(ix, iy, 1)),
                mx_bits_to_01(mx_hash_int(ix, iy, 2)));
}

float3 mx_cell_noise_vec3(float3 p)
{
  uint a, b, c;
  a = b = c = uint(0xdeadbeef) + (4u << 2u) + 13u;
  a += uint(int(floor(p.x)));
  b += uint(int(floor(p.y)));
  c += uint(int(floor(p.z)));
  mx_bjmix(a, b, c);
  return float3(mx_bits_to_01(mx_bjfinal(a, b, c)),
                mx_bits_to_01(mx_bjfinal(a + 1u, b, c)),
                mx_bits_to_01(mx_bjfinal(a + 2u, b, c)));
}

float mx_fractal_noise_float(float2 p, int octaves, float lacunarity, float diminish)
{
  float result = 0.0f;
  float amplitude = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    result += amplitude * mx_perlin_noise_float(p);
    amplitude *= diminish;
    p *= lacunarity;
  }
  return result;
}

float mx_fractal_noise_float(float3 p, int octaves, float lacunarity, float diminish)
{
  float result = 0.0f;
  float amplitude = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    result += amplitude * mx_perlin_noise_float(p);
    amplitude *= diminish;
    p *= lacunarity;
  }
  return result;
}

float3 mx_fractal_noise_vec3(float2 p, int octaves, float lacunarity, float diminish)
{
  float3 result = float3(0.0f);
  float amplitude = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    result += amplitude * mx_perlin_noise_vec3(p);
    amplitude *= diminish;
    p *= lacunarity;
  }
  return result;
}

float3 mx_fractal_noise_vec3(float3 p, int octaves, float lacunarity, float diminish)
{
  float3 result = float3(0.0f);
  float amplitude = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    result += amplitude * mx_perlin_noise_vec3(p);
    amplitude *= diminish;
    p *= lacunarity;
  }
  return result;
}

float2 mx_worley_cell_position(int x, int y, int xoff, int yoff, float jitter)
{
  float3 tmp = mx_cell_noise_vec3(float2(float(x + xoff), float(y + yoff)));
  float2 off = (tmp.xy - float2(0.5f)) * jitter + float2(0.5f);
  return float2(float(x), float(y)) + off;
}

float3 mx_worley_cell_position(int x, int y, int z, int xoff, int yoff, int zoff, float jitter)
{
  float3 off = mx_cell_noise_vec3(float3(float(x + xoff), float(y + yoff), float(z + zoff)));
  off = (off - float3(0.5f)) * jitter + float3(0.5f);
  return float3(float(x), float(y), float(z)) + off;
}

float mx_worley_distance(float2 p, int x, int y, int xoff, int yoff, float jitter, int metric)
{
  float2 diff = mx_worley_cell_position(x, y, xoff, yoff, jitter) - p;
  if (metric == 2) {
    return abs(diff.x) + abs(diff.y);
  }
  if (metric == 3) {
    return max(abs(diff.x), abs(diff.y));
  }
  return dot(diff, diff);
}

float mx_worley_distance(float3 p, int x, int y, int z, int xoff, int yoff, int zoff, float jitter, int metric)
{
  float3 diff = mx_worley_cell_position(x, y, z, xoff, yoff, zoff, jitter) - p;
  if (metric == 2) {
    return abs(diff.x) + abs(diff.y) + abs(diff.z);
  }
  if (metric == 3) {
    return max(max(abs(diff.x), abs(diff.y)), abs(diff.z));
  }
  return dot(diff, diff);
}

float mx_worley_noise_float(float2 p, float jitter, int style, int metric)
{
  int X, Y;
  float2 localpos = float2(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y));
  float sqdist = 1e6f;
  float2 minpos = float2(0.0f);
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      float dist = mx_worley_distance(localpos, x, y, X, Y, jitter, metric);
      float2 cellpos = mx_worley_cell_position(x, y, X, Y, jitter) - localpos;
      if (dist < sqdist) {
        sqdist = dist;
        minpos = cellpos;
      }
    }
  }
  if (style == 1) {
    return mx_cell_noise_float(minpos + p);
  }
  return metric == 0 ? sqrt(sqdist) : sqdist;
}

float mx_worley_noise_float(float3 p, float jitter, int style, int metric)
{
  int X, Y, Z;
  float3 localpos = float3(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y), mx_floorfrac(p.z, Z));
  float sqdist = 1e6f;
  float3 minpos = float3(0.0f);
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      for (int z = -1; z <= 1; ++z) {
        float dist = mx_worley_distance(localpos, x, y, z, X, Y, Z, jitter, metric);
        float3 cellpos = mx_worley_cell_position(x, y, z, X, Y, Z, jitter) - localpos;
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
  return metric == 0 ? sqrt(sqdist) : sqdist;
}

float3 mx_worley_noise_vec3(float2 p, float jitter, int style, int metric)
{
  int X, Y;
  float2 localpos = float2(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y));
  float3 sqdist = float3(1e6f);
  float2 minpos = float2(0.0f);
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      float dist = mx_worley_distance(localpos, x, y, X, Y, jitter, metric);
      float2 cellpos = mx_worley_cell_position(x, y, X, Y, jitter) - localpos;
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
  return metric == 0 ? float3(sqrt(sqdist.x), sqrt(sqdist.y), sqrt(sqdist.z)) : sqdist;
}

float3 mx_worley_noise_vec3(float3 p, float jitter, int style, int metric)
{
  int X, Y, Z;
  float3 localpos = float3(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y), mx_floorfrac(p.z, Z));
  float3 sqdist = float3(1e6f);
  float3 minpos = float3(0.0f);
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      for (int z = -1; z <= 1; ++z) {
        float dist = mx_worley_distance(localpos, x, y, z, X, Y, Z, jitter, metric);
        float3 cellpos = mx_worley_cell_position(x, y, z, X, Y, Z, jitter) - localpos;
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
  return metric == 0 ? float3(sqrt(sqdist.x), sqrt(sqdist.y), sqrt(sqdist.z)) : sqdist;
}

float2 mx_rotate2d(float2 in_vector, float amount)
{
  float radians = amount * M_PI / 180.0f;
  float sa = sin(radians);
  float ca = cos(radians);
  return float2(ca * in_vector.x + sa * in_vector.y, -sa * in_vector.x + ca * in_vector.y);
}

float3 mx_rotate3d(float3 in_vector, float amount)
{
  float3 axis = normalize(float3(0.1f, 1.0f, 0.0f));
  float radians = amount * M_PI / 180.0f;
  float s = sin(radians);
  float c = cos(radians);
  return in_vector * c + cross(axis, in_vector) * s + axis * dot(axis, in_vector) * (1.0f - c);
}

float mx_range_float(float value, float out_min, float out_max, bool clamp_output)
{
  float result = out_min + value * (out_max - out_min);
  return clamp_output ? clamp(result, min(out_min, out_max), max(out_min, out_max)) : result;
}

void mx_noise_eval(float dimensions,
                   float noise_type,
                   float3 vector,
                   float amplitude,
                   float pivot,
                   float octaves_float,
                   float lacunarity,
                   float diminish,
                   float jitter,
                   float style_float,
                   float unified_type_float,
                   float3 freq,
                   float3 offset,
                   float out_min,
                   float out_max,
                   float clamp_output_float,
                   out float value,
                   out float3 color)
{
  bool is_2d = dimensions == 2.0f;
  int octaves = clamp(int(octaves_float), 0, 32);
  int style = int(style_float);
  int unified_type = int(unified_type_float);
  float2 p2 = vector.xy;
  float3 p3 = vector;
  value = 0.0f;
  color = float3(0.0f);

  if (noise_type == 4.0f) {
    float cell_jitter = (jitter - 1.0f) * 90000.0f;
    if (is_2d) {
      float2 apply_offset = vector.xy * freq.xy + offset.xy;
      float2 apply_cell_jitter = mx_rotate2d(apply_offset, cell_jitter);
      if (unified_type == 1) {
        value = mx_cell_noise_float(apply_cell_jitter);
        color = mx_cell_noise_vec3(apply_cell_jitter);
      }
      else if (unified_type == 2) {
        value = mx_worley_noise_float(apply_offset, jitter, style, 0);
        color = mx_worley_noise_vec3(apply_offset, jitter, style, 0);
      }
      else if (unified_type == 3) {
        float3 fractal_position = float3(apply_offset.x, apply_offset.y, cell_jitter);
        value = mx_fractal_noise_float(fractal_position, octaves, lacunarity, diminish);
        color = mx_fractal_noise_vec3(fractal_position, octaves, lacunarity, diminish);
      }
      else {
        value = mx_perlin_noise_float(apply_cell_jitter) * 0.5f + 0.5f;
        color = mx_perlin_noise_vec3(apply_cell_jitter) * 0.5f + float3(0.5f);
      }
    }
    else {
      float3 apply_offset = vector * freq + offset;
      float3 apply_cell_jitter = mx_rotate3d(apply_offset, cell_jitter);
      if (unified_type == 1) {
        value = mx_cell_noise_float(apply_cell_jitter);
        color = mx_cell_noise_vec3(apply_cell_jitter);
      }
      else if (unified_type == 2) {
        value = mx_worley_noise_float(apply_offset, jitter, style, 0);
        color = mx_worley_noise_vec3(apply_offset, jitter, style, 0);
      }
      else if (unified_type == 3) {
        value = mx_fractal_noise_float(apply_cell_jitter, octaves, lacunarity, diminish);
        color = mx_fractal_noise_vec3(apply_cell_jitter, octaves, lacunarity, diminish);
      }
      else {
        value = mx_perlin_noise_float(apply_cell_jitter) * 0.5f + 0.5f;
        color = mx_perlin_noise_vec3(apply_cell_jitter) * 0.5f + float3(0.5f);
      }
    }
    bool clamp_output = clamp_output_float != 0.0f;
    value = mx_range_float(value, out_min, out_max, clamp_output);
    color = float3(mx_range_float(color.x, out_min, out_max, clamp_output),
                   mx_range_float(color.y, out_min, out_max, clamp_output),
                   mx_range_float(color.z, out_min, out_max, clamp_output));
  }
  else if (noise_type == 2.0f) {
    value = is_2d ? mx_cell_noise_float(p2) : mx_cell_noise_float(p3);
    color = is_2d ? mx_cell_noise_vec3(p2) : mx_cell_noise_vec3(p3);
  }
  else if (noise_type == 3.0f) {
    value = is_2d ? mx_worley_noise_float(p2, jitter, style, 0) :
                    mx_worley_noise_float(p3, jitter, style, 0);
    color = is_2d ? mx_worley_noise_vec3(p2, jitter, style, 0) :
                    mx_worley_noise_vec3(p3, jitter, style, 0);
  }
  else if (noise_type == 1.0f) {
    value = is_2d ? mx_fractal_noise_float(p2, octaves, lacunarity, diminish) :
                    mx_fractal_noise_float(p3, octaves, lacunarity, diminish);
    color = is_2d ? mx_fractal_noise_vec3(p2, octaves, lacunarity, diminish) :
                    mx_fractal_noise_vec3(p3, octaves, lacunarity, diminish);
    value *= amplitude;
    color *= amplitude;
  }
  else {
    value = is_2d ? mx_perlin_noise_float(p2) : mx_perlin_noise_float(p3);
    color = is_2d ? mx_perlin_noise_vec3(p2) : mx_perlin_noise_vec3(p3);
    value = value * amplitude + pivot;
    color = color * amplitude + float3(pivot);
  }
}

[[node]]
void node_mx_noise(float3 vector,
                   float amplitude,
                   float pivot,
                   float dimensions,
                   float &value,
                   float4 &color)
{
  float3 color3;
  mx_noise_eval(dimensions,
                0.0f,
                vector,
                amplitude,
                pivot,
                3.0f,
                2.0f,
                0.5f,
                1.0f,
                0.0f,
                0.0f,
                float3(1.0f),
                float3(0.0f),
                0.0f,
                1.0f,
                1.0f,
                value,
                color3);
  color = float4(color3, 1.0f);
}

[[node]]
void node_mx_fractal_noise(float3 vector,
                           float amplitude,
                           float octaves,
                           float lacunarity,
                           float diminish,
                           float dimensions,
                           float &value,
                           float4 &color)
{
  float3 color3;
  mx_noise_eval(dimensions,
                1.0f,
                vector,
                amplitude,
                0.0f,
                octaves,
                lacunarity,
                diminish,
                1.0f,
                0.0f,
                0.0f,
                float3(1.0f),
                float3(0.0f),
                0.0f,
                1.0f,
                1.0f,
                value,
                color3);
  color = float4(color3, 1.0f);
}

[[node]]
void node_mx_cell_noise(float3 vector, float dimensions, float &value, float4 &color)
{
  float3 color3;
  mx_noise_eval(dimensions,
                2.0f,
                vector,
                1.0f,
                0.0f,
                3.0f,
                2.0f,
                0.5f,
                1.0f,
                0.0f,
                0.0f,
                float3(1.0f),
                float3(0.0f),
                0.0f,
                1.0f,
                1.0f,
                value,
                color3);
  color = float4(color3, 1.0f);
}

[[node]]
void node_mx_worley_noise(float3 vector,
                          float jitter,
                          float style,
                          float dimensions,
                          float &value,
                          float4 &color)
{
  float3 color3;
  mx_noise_eval(dimensions,
                3.0f,
                vector,
                1.0f,
                0.0f,
                3.0f,
                2.0f,
                0.5f,
                jitter,
                style,
                0.0f,
                float3(1.0f),
                float3(0.0f),
                0.0f,
                1.0f,
                1.0f,
                value,
                color3);
  color = float4(color3, 1.0f);
}

[[node]]
void node_mx_unified_noise(float3 vector,
                           float3 frequency,
                           float3 offset,
                           float type,
                           float jitter,
                           float style,
                           float octaves,
                           float lacunarity,
                           float diminish,
                           float out_min,
                           float out_max,
                           float clamp_output,
                           float dimensions,
                           float &value,
                           float4 &color)
{
  float3 color3;
  mx_noise_eval(dimensions,
                4.0f,
                vector,
                1.0f,
                0.0f,
                octaves,
                lacunarity,
                diminish,
                jitter,
                style,
                type,
                frequency,
                offset,
                out_min,
                out_max,
                clamp_output,
                value,
                color3);
  color = float4(color3, 1.0f);
}
