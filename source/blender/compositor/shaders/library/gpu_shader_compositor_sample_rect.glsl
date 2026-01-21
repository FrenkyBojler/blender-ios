/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_lib.glsl"

enum Sampler : uchar {
  Nearest,
  Bilinear,
  Box,
  Bspline,
  Anisotropic
};

template<enum Sampler sampler> float4 sample(sampler2D source, float2 uv, float2 wh) {}

template <> float4 sample<Bilinear>(sampler2D source, float2 uv, float2 wh)
{
  return texture(source, uv / float2(textureSize(source, 0)));
}

// Sample orthogonal rectangle of size wh centered on uv.
template <> float4 sample<Box>(sampler2D source, float2 uv, float2 wh)
{
  float2 r = max((wh + 1) / 2.0f, 1.0f);
  float2 d = ceil(r / 8.0f);
  float2 a = (floor(uv - r + 0.5f) + 0.5f);                 // first non-zero sample
  float2 scale = 1.0f / float2(textureSize(source, 0));  // convert to texture coordinates
  // precompute the horizontal filter so it can be reused
  float2 xfilter[33];  // pairs of u,weight
  float divx = 0.0f;
  int nx = 0;
  for (float x = a.x - uv.x; x < r.x; x += 2 * d.x) {
    float weight = min(r.x - abs(x), 1.0f);
    float x2 = x + 1.0f;  // next pixel over
    float weight2 = clamp(r.x - abs(x2), 0.0f, 1.0f);
    weight += weight2;
    x2 = (x + uv.x + weight2 / weight) * scale.x;
    xfilter[nx++] = float2(x2, weight);
    divx += weight;
  }
  float4 sum = float4(0.0f);
  float div = 0.0f;
  for (float x = a.y - uv.y; x < r.y; x += 2 * d.y) {
    float weight = min(r.y - abs(x), 1.0f);
    float x2 = x + 1.0f;
    float weight2 = clamp(r.y - abs(x2), 0.0f, 1.0f);
    weight += weight2;
    x2 = (x + uv.y + weight2 / weight) * scale.y;
    float4 sumx = float4(0.0f);
    for (int j = 0; j < nx; j++) {
      sumx += texture(source, float2(xfilter[j].x, x2)) * xfilter[j].y;
    }
    sum += sumx * weight;
    div += weight;
  }
  return sum / (div * divx);
}

template <enum Sampler sampler> float weight(float x) {}

template <> static inline float weight<Bspline>(float x)
{
  return x < 1 ? (0.5 * x - 1) * x * x + 4.0 / 6 : ((-1 / 6.0 * x + 1) * x - 2) * x + 4.0 / 3;
}

template <> float4 sample<Bspline>(sampler2D source, float2 uv, float2 wh)
{
  float2 w1 = max(wh, 1.0f);
  float2 d = ceil(w1 / 8.0f);
  float2 r = 2 * w1;
  float2 a = (floor(uv - r + 0.5f) + 0.5f);                 // first non-zero sample
  float2 scale = 1.0f / float2(textureSize(source, 0));  // convert to texture coordinates
  // precompute the horizontal filter so it can be reused
  float2 xfilter[33];  // pairs of u,weight
  float divx = 0.0f;
  int nx = 0;
  for (float x = a.x - uv.x; x < r.x; x += 2 * d.x) {
    float weight = weight<Bspline>(abs(x / w1.x));
    float x2 = x + 1.0f;  // next pixel over
    float weight2 = x2 < r.x ? weight<Bspline>(abs(x2 / w1.x)) : 0.0f;
    weight += weight2;
    x2 = (x + uv.x + weight2 / weight) * scale.x;
    xfilter[nx++] = float2(x2, weight);
    divx += weight;
  }
  float4 sum = float4(0.0f);
  float div = 0.0f;
  for (float x = a.y - uv.y; x < r.y; x += 2 * d.y) {
    float weight = weight<Bspline>(abs(x / w1.y));
    float x2 = x + 1.0f;
    float weight2 = x2 < r.y ? weight<Bspline>(abs(x2 / w1.y)) : 0.0f;
    weight += weight2;
    x2 = (x + uv.y + weight2 / weight) * scale.y;
    float4 sumx = float4(0.0f);
    for (int j = 0; j < nx; j++) {
      sumx += texture(source, float2(xfilter[j].x, x2)) * xfilter[j].y;
    }
    sum += sumx * weight;
    div += weight;
  }
  return sum / (div * divx);
}


#if 0 /* potential other samplers */

static inline float weight_triangle(float x, float w)
{
  return 1 - abs(x / w);
}

static inline float weight_cubic(float x, float w)
{
  x = abs(x / w);
  return x < 1 ? (1.5 * x - 15.0 / 6) * x * x + 1 : ((-0.5 * x + 15.0 / 6) * x - 4) * x + 2;
}

static inline float weight_mitchell(float x, float w)
{
  x = abs(x / w);
  return x < 1 ? (7.0 / 6 * x - 2) * x * x + 16.0 / 18 :
                 ((-7.0 / 18 * x + 2) * x - 20.0 / 6) * x + 32.0 / 18;
}

static inline float weight_lanczos5(float x, float w)
{
  x = 3.1415926535897932f * abs(x / w);
  return x != 0 ? sin(x) * sin(x / 5) * 5 / (x * x) : 1;
}

#endif

/* Compute "sharp" black border. The source should be set to extend. */
template<enum Sampler sampler> float4 sample_clip(sampler2D source, float2 uv, float2 wh, int clip)
{
  float m = 1.0f;
  if (bool(clip)) {
    float2 v = min(uv, float2(textureSize(source, 0)) - uv) / wh + 0.5f;
    if (bool(clip & 1)) {
      if (v.x <= 0.0f) {
        return float4(0.0f);
      }
      if (v.x < 1.0f) {
        m = v.x;
      }
    }
    if (bool(clip & 2)) {
      if (v.y <= 0.0f) {
        return float4(0.0f);
      }
      if (v.y < 1.0f) {
        m *= v.y;
      }
    }
  }
  return m * sample<sampler>(source, uv, wh);
}

template float4 sample_clip<Bilinear>(sampler2D source, float2 uv, float2 wh, int clip);
template float4 sample_clip<Box>(sampler2D source, float2 uv, float2 wh, int clip);
template float4 sample_clip<Bspline>(sampler2D source, float2 uv, float2 wh, int clip);
