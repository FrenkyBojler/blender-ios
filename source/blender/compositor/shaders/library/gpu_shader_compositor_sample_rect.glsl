/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float4 sample_nearest(float2 uv)
{
  return texture(input_tx, uv / float2(textureSize(input_tx, 0)));
  // return texelFetch(input_tx, int2(uv), 0);
}

static inline float weight_box(float x, float w)
{
  return min((w + 1) / 2 - abs(x), 1.0f);
}

// Sample orthogonal rectangle of size wh centered on uv.
float4 sample_box(float2 uv, float2 wh)
{
  float2 w1 = max(wh, 1.0f);
  float2 d = ceil(w1 / 8.0f);
  float2 r = (w1 + 1) / 2.0f;
  float2 a = (ceil(uv - r - 0.5f) + 0.5f);                 // first non-zero sample
  float2 scale = 1.0f / float2(textureSize(input_tx, 0));  // convert to texture coordinates
  // precompute the horizontal filter so it can be reused
  float2 xfilter[33];  // pairs of u,weight
  float divx = 0.0f;
  int nx = 0;
  for (float u = a.x - uv.x; u < r.x; u += 2 * d.x) {
    float weight = weight_box(u, w1.x);
    float u2 = u + 1.0f;  // next pixel over
    float weight2 = u2 < r.x ? weight_box(u2, w1.x) : 0.0f;
    weight += weight2;
    u2 = (u + uv.x + weight2 / weight) * scale.x;
    xfilter[nx++] = float2(u2, weight);
    divx += weight;
  }
  float4 sum = float4(0.0f);
  float div = 0.0f;
  for (float v = a.y - uv.y; v < r.y; v += 2 * d.y) {
    float weight = weight_box(v, w1.y);
    float v2 = v + 1.0f;
    float weight2 = v2 < r.y ? weight_box(v2, w1.y) : 0.0f;
    weight += weight2;
    v2 = (v + uv.y + weight2 / weight) * scale.y;
    float4 sumx = float4(0.0f);
    for (int j = 0; j < nx; j++) {
      sumx += texture(input_tx, float2(xfilter[j].x, v2)) * xfilter[j].y;
    }
    sum += sumx * weight;
    div += weight;
  }
  return sum / (div * divx);
}

static inline float weight_bspline(float x, float w)
{
  x = abs(x / w);
  return x < 1 ? (0.5 * x - 1) * x * x + 4.0 / 6 : ((-1 / 6.0 * x + 1) * x - 2) * x + 4.0 / 3;
}

float4 sample_bspline(float2 uv, float2 wh)
{
  float2 w1 = max(wh, 1.0f);
  float2 d = ceil(w1 / 8.0f);
  float2 r = 2 * w1;
  float2 a = (ceil(uv - r - 0.5f) + 0.5f);                 // first non-zero sample
  float2 scale = 1.0f / float2(textureSize(input_tx, 0));  // convert to texture coordinates
  // precompute the horizontal filter so it can be reused
  float2 xfilter[33];  // pairs of u,weight
  float divx = 0.0f;
  int nx = 0;
  for (float u = a.x - uv.x; u < r.x; u += 2 * d.x) {
    float weight = weight_bspline(u, w1.x);
    float u2 = u + 1.0f;  // next pixel over
    float weight2 = u2 < r.x ? weight_bspline(u2, w1.x) : 0.0f;
    weight += weight2;
    u2 = (u + uv.x + weight2 / weight) * scale.x;
    xfilter[nx++] = float2(u2, weight);
    divx += weight;
  }
  float4 sum = float4(0.0f);
  float div = 0.0f;
  for (float v = a.y - uv.y; v < r.y; v += 2 * d.y) {
    float weight = weight_bspline(v, w1.y);
    float v2 = v + 1.0f;
    float weight2 = v2 < r.y ? weight_bspline(v2, w1.y) : 0.0f;
    weight += weight2;
    v2 = (v + uv.y + weight2 / weight) * scale.y;
    float4 sumx = float4(0.0f);
    for (int j = 0; j < nx; j++) {
      sumx += texture(input_tx, float2(xfilter[j].x, v2)) * xfilter[j].y;
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
