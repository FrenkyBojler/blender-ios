/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#if defined(SAMPLER_NEAREST)
float4 sampleRect(float2 uv, float2 wh)
{
  return texture(input_tx, (uv + 0.5f) / float2(textureSize(input_tx, 0)));
  //return texelFetch(input_tx, int2(floor(uv + 0.5f)), 0);
}
#else

// x is distance from center of sample, w is derivative of sample.
// w >= 1, |x| < SAMPLER_RADIUS*w
float samplerWeight(float x, float w)
{
#if defined(SAMPLER_BOX) // matches "bilinear" at w=1
#define SAMPLER_RADIUS(w) (((w)+1)/2)
  return min((w+1)/2-abs(x), 1.0f);

#else
  x = abs(x/w);

#if defined(SAMPLER_TRIANGLE)
#define SAMPLER_RADIUS(w) (w)
  return 1 - x;

#elif defined(SAMPLER_CUBIC)
#define SAMPLER_RADIUS(w) (2*(w))
  return x < 1 ? (1.5*x-15.0/6)*x*x+1 : ((-0.5*x+15.0/6)*x-4)*x+2;

#elif defined(SAMPLER_MITCHELL)
#define SAMPLER_RADIUS(w) (2*(w))
  return x < 1 ? (7.0/6*x-2)*x*x+16.0/18 : ((-7.0/18*x+2)*x-20.0/6)*x+32.0/18;

#elif defined(SAMPLER_BSPLINE) // matches "bicubic" at w=1
#define SAMPLER_RADIUS(w) (2*(w))
  return x < 1 ? (0.5*x-1)*x*x+4.0/6 : ((-1/6.0*x+1)*x-2)*x+4.0/3;

#elif defined(SAMPLER_LANCZOS5)
#define M_PI 3.1415926535897932f
  x = M_PI * x;
#define LOBES 5
#define SAMPLER_RADIUS(w) (LOBES*(w))
  return x != 0 ? sin(x) * sin(x / LOBES) * LOBES / (x*x) : 1;
#endif
#endif
}

// Horizontal sample centered on uv with derivative w, w >= 1
// Integers are at pixel centers
float4 sampleH(float u, int v, float w)
{
  int a = int(ceil(u - SAMPLER_RADIUS(w))); // first non-zero sample
  int e = int(floor(u + SAMPLER_RADIUS(w))); // last non-zero sample
  float4 sum = float4(0.0);
  float4 div = float4(0.0);
  for (; a <= e; a++) {
    float weight = samplerWeight(u-a, w);
    sum += texture(input_tx, (float2(a,v) + 0.5) / float2(textureSize(input_tx, 0))) * weight;
    //sum += texelFetch(input_tx, int2(a,v), 0) * weight;
    div += weight;
  }
  return sum/div;
}

// Sample orthogonal rectangle of size wh centered on xy. wh >= 1
// Integers are at pixel centers
float4 sampleRect(float2 uv, float2 wh)
{
  int a = int(ceil(uv.y - SAMPLER_RADIUS(wh.y)));
  int e = int(floor(uv.y + SAMPLER_RADIUS(wh.y)));
  float4 sum = float4(0.0);
  float4 div = float4(0.0);
  for (; a <= e; a++) {
    float weight = samplerWeight(uv.y-a, wh.y);
    sum += sampleH(uv.x, a, wh.x) * weight;
    div += weight;
  }
  return sum/div;
}

#endif

// this is useful for computing wh
float2 hypot2(float2 a, float2 b)
{
  //return float2(length(float2(a.x,b.x)), length(float2(a.y,b.y)));
  return sqrt(float2(a.x*a.x+b.x*b.x, a.y*a.y+b.y*b.y));
}
