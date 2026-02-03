/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_lib.glsl"

enum Sampler : uchar { Nearest, Bilinear, Box, Bspline, Anisotropic };

template<enum Sampler sampler> inline float weight(float x) {}

/* Sample orthogonal rectangle of size wh centered on uv.
 * Generic version works for any cubic filter (todo: fix for filters with negative weights)
 */
template<enum Sampler sampler> float4 sample_rect(const sampler2D &source, const float2 &uv, const float2 &wh)
{
  const float2 w1 = max(wh, 1.0f);
  const float2 r = 2 * w1;
  const float2 a = (floor(uv - r + 0.5f) + 0.5f);              // first non-zero sample
  const float2 d = ceil(r / 16.0f);                            // distance between samples
  const float2 scale = 1.0f / float2(textureSize(source, 0));  // convert to texture coordinates
  // precompute the horizontal filter so it can be reused
  float2 xfilter[33];  // pairs of u,weight
  float divx = 0.0f;
  int nx = 0;
  for (float x = a.x - uv.x; x < r.x; x += 2 * d.x) {
    float wt = weight<sampler>(abs(x / w1.x));
    float x2 = x + 1.0f;  // next pixel over
    float wt2 = x2 < r.x ? weight<sampler>(abs(x2 / w1.x)) : 0.0f;
    wt += wt2;
    x2 = (x + uv.x + wt2 / wt) * scale.x;
    xfilter[nx++] = float2(x2, wt);
    divx += wt;
  }
  float4 sum = float4(0.0f);
  float div = 0.0f;
  for (float y = a.y - uv.y; y < r.y; y += 2 * d.y) {
    float wt = weight<sampler>(abs(y / w1.y));
    float y2 = y + 1.0f;
    float wt2 = y2 < r.y ? weight<sampler>(abs(y2 / w1.y)) : 0.0f;
    wt += wt2;
    y2 = (y + uv.y + wt2 / wt) * scale.y;
    float4 sumx = float4(0.0f);
    for (int j = 0; j < nx; j++) {
      sumx += texture(source, float2(xfilter[j].x, y2)) * xfilter[j].y;
    }
    sum += sumx * wt;
    div += wt;
  }
  return sum / (div * divx);
}

/* specialized as wh is ignored and it maps directly to texture() */
template<> float4 sample_rect<Sampler::Bilinear>(const sampler2D &source, const float2 &uv, const float2 &wh)
{
  return texture(source, uv / float2(textureSize(source, 0)));
}

/* specialized as r is smaller and weight function needs to know size of a pixel */
template<> float4 sample_rect<Sampler::Box>(const sampler2D &source, const float2 &uv, const float2 &wh)
{
  const float2 r = max((wh + 1) / 2.0f, 1.0f);
  const float2 a = floor(uv - r + 0.5f) + 0.5f;                // first non-zero sample
  const float2 d = ceil(r / 8.0f);                             // distance between samples
  const float2 scale = 1.0f / float2(textureSize(source, 0));  // convert to texture coordinates
  // precompute the horizontal filter so it can be reused
  float2 xfilter[33];  // pairs of u,weight
  float divx = 0.0f;
  int nx = 0;
  for (float x = a.x - uv.x; x < r.x; x += 2 * d.x) {
    float wt = min(r.x - abs(x), 1.0f);
    float x2 = x + 1.0f;  // next pixel over
    float wt2 = clamp(r.x - abs(x2), 0.0f, 1.0f);
    wt += wt2;
    x2 = (x + uv.x + wt2 / wt) * scale.x;
    xfilter[nx++] = float2(x2, wt);
    divx += wt;
  }
  float4 sum = float4(0.0f);
  float div = 0.0f;
  for (float y = a.y - uv.y; y < r.y; y += 2 * d.y) {
    float wt = min(r.y - abs(y), 1.0f);
    float y2 = y + 1.0f;
    float wt2 = clamp(r.y - abs(y2), 0.0f, 1.0f);
    wt += wt2;
    y2 = (y + uv.y + wt2 / wt) * scale.y;
    float4 sumx = float4(0.0f);
    for (int j = 0; j < nx; j++) {
      sumx += texture(source, float2(xfilter[j].x, y2)) * xfilter[j].y;
    }
    sum += sumx * wt;
    div += wt;
  }
  return sum / (div * divx);
}

template<> static inline float weight<Sampler::Bspline>(float x)
{
  return x < 1 ? (0.5 * x - 1) * x * x + 4.0 / 6 : ((-1 / 6.0 * x + 1) * x - 2) * x + 4.0 / 3;
}
template float4 sample_rect<Sampler::Bspline>(sampler2D source, float2 uv, float2 wh);

#if 0 /* potential other samplers */

template <>
static inline float weight<Sampler::Cubic>(float x)
{
  return x < 1 ? (1.5 * x - 15.0 / 6) * x * x + 1 : ((-0.5 * x + 15.0 / 6) * x - 4) * x + 2;
}

template <>
static inline float weight<Sampler::Mitchell>(float x)
{
  return x < 1 ? (7.0 / 6 * x - 2) * x * x + 16.0 / 18 :
                 ((-7.0 / 18 * x + 2) * x - 20.0 / 6) * x + 32.0 / 18;
}

/* r = 5*w1 */
template <>
static inline float weight<Sampler::Lanczos5>(float x)
{
  x = 3.1415926535897932f * x;
  return x != 0 ? sin(x) * sin(x / 5) * 5 / (x * x) : 1;
}

#endif
