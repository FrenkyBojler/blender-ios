/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#if defined(SAMPLER_NEAREST)
float4 sample_rect(float2 uv, float2 wh)
{
  return texture(input_tx, uv / float2(textureSize(input_tx, 0)));
  // return texelFetch(input_tx, int2(uv), 0);
}
#else

// x is distance from center of sample, w is derivative of sample.
// w >= 1, |x| < SAMPLER_RADIUS*w
float samplerWeight(float x, float w)
{
#  if defined(SAMPLER_BOX)  // matches "bilinear" at w=1
#    define SAMPLER_RADIUS(w) (((w) + 1) / 2)
  return min((w + 1) / 2 - abs(x), 1.0f);

#  else
  x = abs(x / w);

#    if defined(SAMPLER_TRIANGLE)
#      define SAMPLER_RADIUS(w) (w)
  return 1 - x;

#    elif defined(SAMPLER_CUBIC)
#      define SAMPLER_RADIUS(w) (2 * (w))
  return x < 1 ? (1.5 * x - 15.0 / 6) * x * x + 1 : ((-0.5 * x + 15.0 / 6) * x - 4) * x + 2;

#    elif defined(SAMPLER_MITCHELL)
#      define SAMPLER_RADIUS(w) (2 * (w))
  return x < 1 ? (7.0 / 6 * x - 2) * x * x + 16.0 / 18 :
                 ((-7.0 / 18 * x + 2) * x - 20.0 / 6) * x + 32.0 / 18;

#    elif defined(SAMPLER_BSPLINE)  // matches "bicubic" at w=1
#      define SAMPLER_RADIUS(w) (2 * (w))
  return x < 1 ? (0.5 * x - 1) * x * x + 4.0 / 6 : ((-1 / 6.0 * x + 1) * x - 2) * x + 4.0 / 3;

#    elif defined(SAMPLER_LANCZOS5)
#      define M_PI 3.1415926535897932f
  x = M_PI * x;
#      define LOBES 5
#      define SAMPLER_RADIUS(w) (LOBES * (w))
  return x != 0 ? sin(x) * sin(x / LOBES) * LOBES / (x * x) : 1;
#    endif
#  endif
}

#  define MAX_PER_RADIUS 8.0f
#  define MAX_SAMPLES 33 /* 4 * MAX_PER_RADIUS + 1 */

// Sample orthogonal rectangle of size wh centered on uv.
// Integers are at pixel corners
float4 sample_rect(float2 uv, float2 wh)
{
  float2 w1 = max(wh, 1.0f);
  float2 d = ceil(w1 / MAX_PER_RADIUS);
  float2 r = SAMPLER_RADIUS(w1);
  float2 a = (ceil(uv - r - 0.5f) + 0.5f);                 // first non-zero sample
  float2 scale = 1.0f / float2(textureSize(input_tx, 0));  // convert to texture coordinates
  // precompute the horizontal filter so it can be reused
  float2 xfilter[MAX_SAMPLES];  // pairs of u,weight
  float divx = 0.0f;
  int nx = 0;
  for (float u = a.x - uv.x; u < r.x; u += d.x) {
    float weight = samplerWeight(u, w1.x);
    float u2 = u + 1.0f;  // next pixel over
    float weight2 = u2 < r.x ? samplerWeight(u2, w1.x) : 0.0f;
    if (weight * weight2 < 0.0f) {  // does not work if weights have different signs
      u2 = u;
    }
    else {
      weight += weight2;
      u2 = u + weight2 / weight;
      u += d.x;
    }
    xfilter[nx++] = float2((u2 + uv.x) * scale.x, weight);
    divx += weight;
  }
  float4 sum = float4(0.0f);
  float div = 0.0f;
  for (float v = a.y - uv.y; v < r.y; v += d.y) {
    float weight = samplerWeight(v, w1.y);
    float v2 = v + 1.0f;
    float weight2 = v2 < r.y ? samplerWeight(v2, w1.y) : 0.0f;
    if (weight * weight2 < 0.0f) {
      v2 = v;
    }
    else {
      weight += weight2;
      v2 = v + weight2 / weight;
      v += d.y;
    }
    v2 = (v2 + uv.y) * scale.y;
    float4 sumx = float4(0.0f);
    for (int j = 0; j < nx; j++) {
      sumx += texture(input_tx, float2(xfilter[j].x, v2)) * xfilter[j].y;
    }
    sum += sumx * weight;
    div += weight;
  }
  return sum / (div * divx);
}

#endif
