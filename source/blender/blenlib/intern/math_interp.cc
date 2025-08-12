/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_interp.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_simd.hh"

#include "BLI_strict_flags.h" /* IWYU pragma: keep. Keep last. */

namespace blender::math {

BLI_INLINE int wrap_coord(float u, int size, InterpWrapMode wrap)
{
  int x = 0;
  switch (wrap) {
    case InterpWrapMode::Extend:
      x = math::clamp(int(u), 0, size - 1);
      break;
    case InterpWrapMode::Repeat:
      x = int(floored_fmod(u, float(size)));
      break;
    case InterpWrapMode::Border:
      x = int(u);
      if (u < 0.0f || x >= size) {
        x = -1;
      }
      break;
  }
  return x;
}

void interpolate_nearest_wrapmode_fl(const float *buffer,
                                     float *output,
                                     int width,
                                     int height,
                                     int components,
                                     float u,
                                     float v,
                                     InterpWrapMode wrap_u,
                                     InterpWrapMode wrap_v)
{
  BLI_assert(buffer);
  int x = wrap_coord(u, width, wrap_u);
  int y = wrap_coord(v, height, wrap_v);
  if (x < 0 || y < 0) {
    for (int i = 0; i < components; i++) {
      output[i] = 0.0f;
    }
    return;
  }

  const float *data = buffer + (int64_t(width) * y + x) * components;
  for (int i = 0; i < components; i++) {
    output[i] = data[i];
  }
}

enum class eCubicFilter {
  BSpline,
  Mitchell,
};

/* Calculate cubic filter coefficients, for samples at -1,0,+1,+2.
 * f is 0..1 offset from texel center in pixel space. */
template<enum eCubicFilter filter> static float4 cubic_filter_coefficients(float f)
{
  float f2 = f * f;
  float f3 = f2 * f;

  if constexpr (filter == eCubicFilter::BSpline) {
    /* Cubic B-Spline (Mitchell-Netravali filter with B=1, C=0 parameters). */
    float w3 = f3 * (1.0f / 6.0f);
    float w0 = -w3 + f2 * 0.5f - f * 0.5f + 1.0f / 6.0f;
    float w1 = f3 * 0.5f - f2 * 1.0f + 2.0f / 3.0f;
    float w2 = 1.0f - w0 - w1 - w3;
    return float4(w0, w1, w2, w3);
  }
  else if constexpr (filter == eCubicFilter::Mitchell) {
    /* Cubic Mitchell-Netravali filter with B=1/3, C=1/3 parameters. */
    float w0 = -7.0f / 18.0f * f3 + 5.0f / 6.0f * f2 - 0.5f * f + 1.0f / 18.0f;
    float w1 = 7.0f / 6.0f * f3 - 2.0f * f2 + 8.0f / 9.0f;
    float w2 = -7.0f / 6.0f * f3 + 3.0f / 2.0f * f2 + 0.5f * f + 1.0f / 18.0f;
    float w3 = 7.0f / 18.0f * f3 - 1.0f / 3.0f * f2;
    return float4(w0, w1, w2, w3);
  }
}

#if BLI_HAVE_SSE4
template<eCubicFilter filter>
BLI_INLINE void bicubic_interpolation_uchar_simd(
    const uchar *src_buffer, uchar *output, int width, int height, float u, float v)
{
  __m128 uv = _mm_set_ps(0, 0, v, u);
  __m128 uv_floor = _mm_floor_ps(uv);
  __m128i i_uv = _mm_cvttps_epi32(uv_floor);

  __m128 frac_uv = _mm_sub_ps(uv, uv_floor);

  /* Calculate pixel weights. */
  float4 wx = cubic_filter_coefficients<filter>(_mm_cvtss_f32(frac_uv));
  float4 wy = cubic_filter_coefficients<filter>(
      _mm_cvtss_f32(_mm_shuffle_ps(frac_uv, frac_uv, 1)));

  /* Read 4x4 source pixels and blend them. */
  __m128 out = _mm_setzero_ps();
  int iu = _mm_cvtsi128_si32(i_uv);
  int iv = _mm_cvtsi128_si32(_mm_shuffle_epi32(i_uv, 1));

  for (int n = 0; n < 4; n++) {
    int y1 = iv + n - 1;
    CLAMP(y1, 0, height - 1);
    for (int m = 0; m < 4; m++) {

      int x1 = iu + m - 1;
      CLAMP(x1, 0, width - 1);
      float w = wx[m] * wy[n];

      const uchar *data = src_buffer + (width * y1 + x1) * 4;
      /* Load 4 bytes and expand into 4-lane SIMD. */
      __m128i sample_i = _mm_castps_si128(_mm_load_ss((const float *)data));
      sample_i = _mm_unpacklo_epi8(sample_i, _mm_setzero_si128());
      sample_i = _mm_unpacklo_epi16(sample_i, _mm_setzero_si128());

      /* Accumulate into out with weight. */
      out = _mm_add_ps(out, _mm_mul_ps(_mm_cvtepi32_ps(sample_i), _mm_set1_ps(w)));
    }
  }

  /* Pack and write to destination: pack to 16 bit signed, then to 8 bit
   * unsigned, then write resulting 32-bit value. This will clamp
   * out of range values too. */
  out = _mm_add_ps(out, _mm_set1_ps(0.5f));
  __m128i rgba32 = _mm_cvttps_epi32(out);
  __m128i rgba16 = _mm_packs_epi32(rgba32, _mm_setzero_si128());
  __m128i rgba8 = _mm_packus_epi16(rgba16, _mm_setzero_si128());
  _mm_store_ss((float *)output, _mm_castsi128_ps(rgba8));
}
#endif /* BLI_HAVE_SSE4 */

template<typename T, eCubicFilter filter>
BLI_INLINE void bicubic_interpolation(const T *src_buffer,
                                      T *output,
                                      int width,
                                      int height,
                                      int components,
                                      float u,
                                      float v,
                                      InterpWrapMode wrap_u,
                                      InterpWrapMode wrap_v)
{
  BLI_assert(src_buffer && output);
  BLI_assert(components > 0 && components <= 4);

#if BLI_HAVE_SSE4
  if constexpr (std::is_same_v<T, uchar>) {
    if (components == 4 && wrap_u == InterpWrapMode::Extend && wrap_v == InterpWrapMode::Extend) {
      bicubic_interpolation_uchar_simd<filter>(src_buffer, output, width, height, u, v);
      return;
    }
  }
#endif

  int iu = int(floor(u));
  int iv = int(floor(v));

  /* Sample area entirely outside image in border mode? */
  if (wrap_u == InterpWrapMode::Border && (iu + 2 < 0 || iu > width)) {
    memset(output, 0, size_t(components) * sizeof(T));
    return;
  }
  if (wrap_v == InterpWrapMode::Border && (iv + 2 < 0 || iv > height)) {
    memset(output, 0, size_t(components) * sizeof(T));
    return;
  }

  float frac_u = u - float(iu);
  float frac_v = v - float(iv);

  float4 out{0.0f};

  /* Calculate pixel weights. */
  float4 wx = cubic_filter_coefficients<filter>(frac_u);
  float4 wy = cubic_filter_coefficients<filter>(frac_v);

  /* Read 4x4 source pixels and blend them. */
  for (int n = 0; n < 4; n++) {
    int y1 = iv + n - 1;
    y1 = wrap_coord(float(y1), height, wrap_v);
    if (wrap_v == InterpWrapMode::Border && y1 < 0) {
      continue;
    }

    for (int m = 0; m < 4; m++) {
      int x1 = iu + m - 1;
      x1 = wrap_coord(float(x1), width, wrap_u);
      if (wrap_u == InterpWrapMode::Border && x1 < 0) {
        continue;
      }
      float w = wx[m] * wy[n];

      const T *data = src_buffer + (width * y1 + x1) * components;

      if (components == 1) {
        out[0] += data[0] * w;
      }
      else if (components == 2) {
        out[0] += data[0] * w;
        out[1] += data[1] * w;
      }
      else if (components == 3) {
        out[0] += data[0] * w;
        out[1] += data[1] * w;
        out[2] += data[2] * w;
      }
      else {
        out[0] += data[0] * w;
        out[1] += data[1] * w;
        out[2] += data[2] * w;
        out[3] += data[3] * w;
      }
    }
  }

  /* Mitchell filter has negative lobes; prevent output from going out of range. */
  if constexpr (filter == eCubicFilter::Mitchell) {
    for (int i = 0; i < components; i++) {
      out[i] = math::max(out[i], 0.0f);
      if constexpr (std::is_same_v<T, uchar>) {
        out[i] = math::min(out[i], 255.0f);
      }
    }
  }

  /* Write result. */
  if constexpr (std::is_same_v<T, float>) {
    if (components == 1) {
      output[0] = out[0];
    }
    else if (components == 2) {
      copy_v2_v2(output, out);
    }
    else if (components == 3) {
      copy_v3_v3(output, out);
    }
    else {
      copy_v4_v4(output, out);
    }
  }
  else {
    if (components == 1) {
      output[0] = uchar(out[0] + 0.5f);
    }
    else if (components == 2) {
      output[0] = uchar(out[0] + 0.5f);
      output[1] = uchar(out[1] + 0.5f);
    }
    else if (components == 3) {
      output[0] = uchar(out[0] + 0.5f);
      output[1] = uchar(out[1] + 0.5f);
      output[2] = uchar(out[2] + 0.5f);
    }
    else {
      output[0] = uchar(out[0] + 0.5f);
      output[1] = uchar(out[1] + 0.5f);
      output[2] = uchar(out[2] + 0.5f);
      output[3] = uchar(out[3] + 0.5f);
    }
  }
}

BLI_INLINE void bilinear_fl_impl(const float *buffer,
                                 float *output,
                                 int width,
                                 int height,
                                 int components,
                                 float u,
                                 float v,
                                 InterpWrapMode wrap_x,
                                 InterpWrapMode wrap_y)
{
  BLI_assert(buffer && output);
  BLI_assert(components > 0 && components <= 4);

  float a, b;
  float a_b, ma_b, a_mb, ma_mb;
  int y1, y2, x1, x2;

  if (wrap_x == InterpWrapMode::Repeat) {
    u = floored_fmod(u, float(width));
  }
  if (wrap_y == InterpWrapMode::Repeat) {
    v = floored_fmod(v, float(height));
  }

  float uf = floorf(u);
  float vf = floorf(v);

  x1 = int(uf);
  x2 = x1 + 1;
  y1 = int(vf);
  y2 = y1 + 1;

  const float *row1, *row2, *row3, *row4;
  const float empty[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  /* Check if +1 samples need wrapping, or we don't do wrapping then if
   * we are sampling completely outside the image. */
  if (wrap_x == InterpWrapMode::Repeat) {
    if (x2 >= width) {
      x2 = 0;
    }
  }
  else if (wrap_x == InterpWrapMode::Border && (x2 < 0 || x1 >= width)) {
    copy_vn_fl(output, components, 0.0f);
    return;
  }
  if (wrap_y == InterpWrapMode::Repeat) {
    if (y2 >= height) {
      y2 = 0;
    }
  }
  else if (wrap_y == InterpWrapMode::Border && (y2 < 0 || y1 >= height)) {
    copy_vn_fl(output, components, 0.0f);
    return;
  }

  /* Sample locations. */
  int x1c = blender::math::clamp(x1, 0, width - 1);
  int x2c = blender::math::clamp(x2, 0, width - 1);
  int y1c = blender::math::clamp(y1, 0, height - 1);
  int y2c = blender::math::clamp(y2, 0, height - 1);
  row1 = buffer + (int64_t(width) * y1c + x1c) * components;
  row2 = buffer + (int64_t(width) * y2c + x1c) * components;
  row3 = buffer + (int64_t(width) * y1c + x2c) * components;
  row4 = buffer + (int64_t(width) * y2c + x2c) * components;

  if (wrap_x == InterpWrapMode::Border) {
    if (x1 < 0) {
      row1 = empty;
      row2 = empty;
    }
    if (x2 > width - 1) {
      row3 = empty;
      row4 = empty;
    }
  }
  if (wrap_y == InterpWrapMode::Border) {
    if (y1 < 0) {
      row1 = empty;
      row3 = empty;
    }
    if (y2 > height - 1) {
      row2 = empty;
      row4 = empty;
    }
  }

  /* Finally, do interpolation. */
  a = u - uf;
  b = v - vf;
  a_b = a * b;
  ma_b = (1.0f - a) * b;
  a_mb = a * (1.0f - b);
  ma_mb = (1.0f - a) * (1.0f - b);

  if (components == 1) {
    output[0] = ma_mb * row1[0] + a_mb * row3[0] + ma_b * row2[0] + a_b * row4[0];
  }
  else if (components == 2) {
    output[0] = ma_mb * row1[0] + a_mb * row3[0] + ma_b * row2[0] + a_b * row4[0];
    output[1] = ma_mb * row1[1] + a_mb * row3[1] + ma_b * row2[1] + a_b * row4[1];
  }
  else if (components == 3) {
    output[0] = ma_mb * row1[0] + a_mb * row3[0] + ma_b * row2[0] + a_b * row4[0];
    output[1] = ma_mb * row1[1] + a_mb * row3[1] + ma_b * row2[1] + a_b * row4[1];
    output[2] = ma_mb * row1[2] + a_mb * row3[2] + ma_b * row2[2] + a_b * row4[2];
  }
  else {
#if BLI_HAVE_SSE2
    __m128 rgba1 = _mm_loadu_ps(row1);
    __m128 rgba2 = _mm_loadu_ps(row2);
    __m128 rgba3 = _mm_loadu_ps(row3);
    __m128 rgba4 = _mm_loadu_ps(row4);
    rgba1 = _mm_mul_ps(_mm_set1_ps(ma_mb), rgba1);
    rgba2 = _mm_mul_ps(_mm_set1_ps(ma_b), rgba2);
    rgba3 = _mm_mul_ps(_mm_set1_ps(a_mb), rgba3);
    rgba4 = _mm_mul_ps(_mm_set1_ps(a_b), rgba4);
    __m128 rgba13 = _mm_add_ps(rgba1, rgba3);
    __m128 rgba24 = _mm_add_ps(rgba2, rgba4);
    __m128 rgba = _mm_add_ps(rgba13, rgba24);
    _mm_storeu_ps(output, rgba);
#else
    output[0] = ma_mb * row1[0] + a_mb * row3[0] + ma_b * row2[0] + a_b * row4[0];
    output[1] = ma_mb * row1[1] + a_mb * row3[1] + ma_b * row2[1] + a_b * row4[1];
    output[2] = ma_mb * row1[2] + a_mb * row3[2] + ma_b * row2[2] + a_b * row4[2];
    output[3] = ma_mb * row1[3] + a_mb * row3[3] + ma_b * row2[3] + a_b * row4[3];
#endif
  }
}

template<bool border>
BLI_INLINE uchar4 bilinear_byte_impl(const uchar *buffer, int width, int height, float u, float v)
{
  BLI_assert(buffer);
  uchar4 res;

#if BLI_HAVE_SSE4
  __m128 uvuv = _mm_set_ps(v, u, v, u);
  __m128 uvuv_floor = _mm_floor_ps(uvuv);

  /* x1, y1, x2, y2 */
  __m128i xy12 = _mm_add_epi32(_mm_cvttps_epi32(uvuv_floor), _mm_set_epi32(1, 1, 0, 0));
  /* Check whether any of the coordinates are outside of the image. */
  __m128i size_minus_1 = _mm_sub_epi32(_mm_set_epi32(height, width, height, width),
                                       _mm_set1_epi32(1));

  /* Samples 1,2,3,4 will be in this order: x1y1, x1y2, x2y1, x2y2. */
  __m128i x1234, y1234, invalid_1234;

  if constexpr (border) {
    /* Blend black colors for samples right outside the image: figure out
     * which of the 4 samples were outside, set their coordinates to zero
     * and later on put black color into their place. */
    __m128i too_lo_xy12 = _mm_cmplt_epi32(xy12, _mm_setzero_si128());
    __m128i too_hi_xy12 = _mm_cmplt_epi32(size_minus_1, xy12);
    __m128i invalid_xy12 = _mm_or_si128(too_lo_xy12, too_hi_xy12);

    /* Samples 1,2,3,4 are in this order: x1y1, x1y2, x2y1, x2y2 */
    x1234 = _mm_shuffle_epi32(xy12, _MM_SHUFFLE(2, 2, 0, 0));
    y1234 = _mm_shuffle_epi32(xy12, _MM_SHUFFLE(3, 1, 3, 1));
    invalid_1234 = _mm_or_si128(_mm_shuffle_epi32(invalid_xy12, _MM_SHUFFLE(2, 2, 0, 0)),
                                _mm_shuffle_epi32(invalid_xy12, _MM_SHUFFLE(3, 1, 3, 1)));
    /* Set x & y to zero for invalid samples. */
    x1234 = _mm_andnot_si128(invalid_1234, x1234);
    y1234 = _mm_andnot_si128(invalid_1234, y1234);
  }
  else {
    /* Clamp samples to image edges. */
    __m128i xy12_clamped = _mm_max_epi32(xy12, _mm_setzero_si128());
    xy12_clamped = _mm_min_epi32(xy12_clamped, size_minus_1);
    x1234 = _mm_shuffle_epi32(xy12_clamped, _MM_SHUFFLE(2, 2, 0, 0));
    y1234 = _mm_shuffle_epi32(xy12_clamped, _MM_SHUFFLE(3, 1, 3, 1));
  }

  /* Read the four sample values. Do address calculations in C, since SSE
   * before 4.1 makes it very cumbersome to do full integer multiplies. */
  int xcoord[4];
  int ycoord[4];
  _mm_storeu_ps((float *)xcoord, _mm_castsi128_ps(x1234));
  _mm_storeu_ps((float *)ycoord, _mm_castsi128_ps(y1234));
  int sample1 = ((const int *)buffer)[ycoord[0] * int64_t(width) + xcoord[0]];
  int sample2 = ((const int *)buffer)[ycoord[1] * int64_t(width) + xcoord[1]];
  int sample3 = ((const int *)buffer)[ycoord[2] * int64_t(width) + xcoord[2]];
  int sample4 = ((const int *)buffer)[ycoord[3] * int64_t(width) + xcoord[3]];
  __m128i samples1234 = _mm_set_epi32(sample4, sample3, sample2, sample1);
  if constexpr (border) {
    /* Set samples to black for the ones that were actually invalid. */
    samples1234 = _mm_andnot_si128(invalid_1234, samples1234);
  }

  /* Expand samples from packed 8-bit RGBA to full floats:
   * spread to 16 bit values. */
  __m128i rgba16_12 = _mm_unpacklo_epi8(samples1234, _mm_setzero_si128());
  __m128i rgba16_34 = _mm_unpackhi_epi8(samples1234, _mm_setzero_si128());
  /* Spread to 32 bit values and convert to float. */
  __m128 rgba1 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(rgba16_12, _mm_setzero_si128()));
  __m128 rgba2 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(rgba16_12, _mm_setzero_si128()));
  __m128 rgba3 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(rgba16_34, _mm_setzero_si128()));
  __m128 rgba4 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(rgba16_34, _mm_setzero_si128()));

  /* Calculate interpolation factors: (1-a)*(1-b), (1-a)*b, a*(1-b), a*b */
  __m128 abab = _mm_sub_ps(uvuv, uvuv_floor);
  __m128 m_abab = _mm_sub_ps(_mm_set1_ps(1.0f), abab);
  __m128 ab_mab = _mm_shuffle_ps(abab, m_abab, _MM_SHUFFLE(3, 2, 1, 0));
  __m128 factors = _mm_mul_ps(_mm_shuffle_ps(ab_mab, ab_mab, _MM_SHUFFLE(0, 0, 2, 2)),
                              _mm_shuffle_ps(ab_mab, ab_mab, _MM_SHUFFLE(1, 3, 1, 3)));

  /* Blend the samples. */
  rgba1 = _mm_mul_ps(_mm_shuffle_ps(factors, factors, _MM_SHUFFLE(0, 0, 0, 0)), rgba1);
  rgba2 = _mm_mul_ps(_mm_shuffle_ps(factors, factors, _MM_SHUFFLE(1, 1, 1, 1)), rgba2);
  rgba3 = _mm_mul_ps(_mm_shuffle_ps(factors, factors, _MM_SHUFFLE(2, 2, 2, 2)), rgba3);
  rgba4 = _mm_mul_ps(_mm_shuffle_ps(factors, factors, _MM_SHUFFLE(3, 3, 3, 3)), rgba4);
  __m128 rgba13 = _mm_add_ps(rgba1, rgba3);
  __m128 rgba24 = _mm_add_ps(rgba2, rgba4);
  __m128 rgba = _mm_add_ps(rgba13, rgba24);
  rgba = _mm_add_ps(rgba, _mm_set1_ps(0.5f));
  /* Pack and write to destination: pack to 16 bit signed, then to 8 bit
   * unsigned, then write resulting 32-bit value. */
  __m128i rgba32 = _mm_cvttps_epi32(rgba);
  __m128i rgba16 = _mm_packs_epi32(rgba32, _mm_setzero_si128());
  __m128i rgba8 = _mm_packus_epi16(rgba16, _mm_setzero_si128());
  _mm_store_ss((float *)&res, _mm_castsi128_ps(rgba8));

#else

  float uf = floorf(u);
  float vf = floorf(v);

  int x1 = int(uf);
  int x2 = x1 + 1;
  int y1 = int(vf);
  int y2 = y1 + 1;

  /* Completely outside of the image in bordered mode? */
  if (border && (x2 < 0 || x1 >= width || y2 < 0 || y1 >= height)) {
    return uchar4(0);
  }

  /* Sample locations. */
  const uchar *row1, *row2, *row3, *row4;
  uchar empty[4] = {0, 0, 0, 0};
  if constexpr (border) {
    row1 = (x1 < 0 || y1 < 0) ? empty : buffer + (int64_t(width) * y1 + x1) * 4;
    row2 = (x1 < 0 || y2 > height - 1) ? empty : buffer + (int64_t(width) * y2 + x1) * 4;
    row3 = (x2 > width - 1 || y1 < 0) ? empty : buffer + (int64_t(width) * y1 + x2) * 4;
    row4 = (x2 > width - 1 || y2 > height - 1) ? empty : buffer + (int64_t(width) * y2 + x2) * 4;
  }
  else {
    x1 = blender::math::clamp(x1, 0, width - 1);
    x2 = blender::math::clamp(x2, 0, width - 1);
    y1 = blender::math::clamp(y1, 0, height - 1);
    y2 = blender::math::clamp(y2, 0, height - 1);
    row1 = buffer + (int64_t(width) * y1 + x1) * 4;
    row2 = buffer + (int64_t(width) * y2 + x1) * 4;
    row3 = buffer + (int64_t(width) * y1 + x2) * 4;
    row4 = buffer + (int64_t(width) * y2 + x2) * 4;
  }

  float a = u - uf;
  float b = v - vf;
  float a_b = a * b;
  float ma_b = (1.0f - a) * b;
  float a_mb = a * (1.0f - b);
  float ma_mb = (1.0f - a) * (1.0f - b);

  res.x = uchar(ma_mb * row1[0] + a_mb * row3[0] + ma_b * row2[0] + a_b * row4[0] + 0.5f);
  res.y = uchar(ma_mb * row1[1] + a_mb * row3[1] + ma_b * row2[1] + a_b * row4[1] + 0.5f);
  res.z = uchar(ma_mb * row1[2] + a_mb * row3[2] + ma_b * row2[2] + a_b * row4[2] + 0.5f);
  res.w = uchar(ma_mb * row1[3] + a_mb * row3[3] + ma_b * row2[3] + a_b * row4[3] + 0.5f);
#endif

  return res;
}

uchar4 interpolate_bilinear_border_byte(
    const uchar *buffer, int width, int height, float u, float v)
{
  return bilinear_byte_impl<true>(buffer, width, height, u, v);
}

uchar4 interpolate_bilinear_byte(const uchar *buffer, int width, int height, float u, float v)
{
  return bilinear_byte_impl<false>(buffer, width, height, u, v);
}

float4 interpolate_bilinear_border_fl(const float *buffer, int width, int height, float u, float v)
{
  float4 res;
  bilinear_fl_impl(
      buffer, res, width, height, 4, u, v, InterpWrapMode::Border, InterpWrapMode::Border);
  return res;
}

void interpolate_bilinear_border_fl(
    const float *buffer, float *output, int width, int height, int components, float u, float v)
{
  bilinear_fl_impl(buffer,
                   output,
                   width,
                   height,
                   components,
                   u,
                   v,
                   InterpWrapMode::Border,
                   InterpWrapMode::Border);
}

float4 interpolate_bilinear_fl(const float *buffer, int width, int height, float u, float v)
{
  float4 res;
  bilinear_fl_impl(
      buffer, res, width, height, 4, u, v, InterpWrapMode::Extend, InterpWrapMode::Extend);
  return res;
}

void interpolate_bilinear_fl(
    const float *buffer, float *output, int width, int height, int components, float u, float v)
{
  bilinear_fl_impl(buffer,
                   output,
                   width,
                   height,
                   components,
                   u,
                   v,
                   InterpWrapMode::Extend,
                   InterpWrapMode::Extend);
}

void interpolate_bilinear_wrapmode_fl(const float *buffer,
                                      float *output,
                                      int width,
                                      int height,
                                      int components,
                                      float u,
                                      float v,
                                      InterpWrapMode wrap_u,
                                      InterpWrapMode wrap_v)
{
  bilinear_fl_impl(buffer, output, width, height, components, u, v, wrap_u, wrap_v);
}

uchar4 interpolate_bilinear_wrap_byte(const uchar *buffer, int width, int height, float u, float v)
{
  u = floored_fmod(u, float(width));
  v = floored_fmod(v, float(height));
  float uf = floorf(u);
  float vf = floorf(v);

  int x1 = int(uf);
  int x2 = x1 + 1;
  int y1 = int(vf);
  int y2 = y1 + 1;

  /* Wrap interpolation pixels if needed. */
  BLI_assert(x1 >= 0 && x1 < width && y1 >= 0 && y1 < height);
  if (x2 >= width) {
    x2 = 0;
  }
  if (y2 >= height) {
    y2 = 0;
  }

  float a = u - uf;
  float b = v - vf;
  float a_b = a * b;
  float ma_b = (1.0f - a) * b;
  float a_mb = a * (1.0f - b);
  float ma_mb = (1.0f - a) * (1.0f - b);

  /* Blend samples. */
  const uchar *row1 = buffer + (int64_t(width) * y1 + x1) * 4;
  const uchar *row2 = buffer + (int64_t(width) * y2 + x1) * 4;
  const uchar *row3 = buffer + (int64_t(width) * y1 + x2) * 4;
  const uchar *row4 = buffer + (int64_t(width) * y2 + x2) * 4;

  uchar4 res;
  res.x = uchar(ma_mb * row1[0] + a_mb * row3[0] + ma_b * row2[0] + a_b * row4[0] + 0.5f);
  res.y = uchar(ma_mb * row1[1] + a_mb * row3[1] + ma_b * row2[1] + a_b * row4[1] + 0.5f);
  res.z = uchar(ma_mb * row1[2] + a_mb * row3[2] + ma_b * row2[2] + a_b * row4[2] + 0.5f);
  res.w = uchar(ma_mb * row1[3] + a_mb * row3[3] + ma_b * row2[3] + a_b * row4[3] + 0.5f);
  return res;
}

float4 interpolate_bilinear_wrap_fl(const float *buffer, int width, int height, float u, float v)
{
  float4 res;
  bilinear_fl_impl(
      buffer, res, width, height, 4, u, v, InterpWrapMode::Repeat, InterpWrapMode::Repeat);
  return res;
}

uchar4 interpolate_cubic_bspline_byte(const uchar *buffer, int width, int height, float u, float v)
{
  uchar4 res;
  bicubic_interpolation<uchar, eCubicFilter::BSpline>(
      buffer, res, width, height, 4, u, v, InterpWrapMode::Extend, InterpWrapMode::Extend);
  return res;
}

float4 interpolate_cubic_bspline_fl(const float *buffer, int width, int height, float u, float v)
{
  float4 res;
  bicubic_interpolation<float, eCubicFilter::BSpline>(
      buffer, res, width, height, 4, u, v, InterpWrapMode::Extend, InterpWrapMode::Extend);
  return res;
}

void interpolate_cubic_bspline_fl(
    const float *buffer, float *output, int width, int height, int components, float u, float v)
{
  bicubic_interpolation<float, eCubicFilter::BSpline>(buffer,
                                                      output,
                                                      width,
                                                      height,
                                                      components,
                                                      u,
                                                      v,
                                                      InterpWrapMode::Extend,
                                                      InterpWrapMode::Extend);
}

void interpolate_cubic_bspline_wrapmode_fl(const float *buffer,
                                           float *output,
                                           int width,
                                           int height,
                                           int components,
                                           float u,
                                           float v,
                                           math::InterpWrapMode wrap_u,
                                           math::InterpWrapMode wrap_v)
{
  bicubic_interpolation<float, eCubicFilter::BSpline>(
      buffer, output, width, height, components, u, v, wrap_u, wrap_v);
}

uchar4 interpolate_cubic_mitchell_byte(
    const uchar *buffer, int width, int height, float u, float v)
{
  uchar4 res;
  bicubic_interpolation<uchar, eCubicFilter::Mitchell>(
      buffer, res, width, height, 4, u, v, InterpWrapMode::Extend, InterpWrapMode::Extend);
  return res;
}

float4 interpolate_cubic_mitchell_fl(const float *buffer, int width, int height, float u, float v)
{
  float4 res;
  bicubic_interpolation<float, eCubicFilter::Mitchell>(
      buffer, res, width, height, 4, u, v, InterpWrapMode::Extend, InterpWrapMode::Extend);
  return res;
}

void interpolate_cubic_mitchell_fl(
    const float *buffer, float *output, int width, int height, int components, float u, float v)
{
  bicubic_interpolation<float, eCubicFilter::Mitchell>(buffer,
                                                       output,
                                                       width,
                                                       height,
                                                       components,
                                                       u,
                                                       v,
                                                       InterpWrapMode::Extend,
                                                       InterpWrapMode::Extend);
}

}  // namespace blender::math

/**************************************************************************
 * Filtering method based on
 * "Creating raster omnimax images from multiple perspective views
 * using the elliptical weighted average filter"
 * by Ned Greene and Paul S. Heckbert (1986)
 ***************************************************************************/

/* Table of `(exp(ar) - exp(a)) / (1 - exp(a))` for `r` in range [0, 1] and `a = -2`.
 * used instead of actual gaussian,
 * otherwise at high texture magnifications circular artifacts are visible. */
#define EWA_MAXIDX 255
const float EWA_WTS[EWA_MAXIDX + 1] = {
    1.0f,        0.990965f,   0.982f,      0.973105f,   0.96428f,    0.955524f,   0.946836f,
    0.938216f,   0.929664f,   0.921178f,   0.912759f,   0.904405f,   0.896117f,   0.887893f,
    0.879734f,   0.871638f,   0.863605f,   0.855636f,   0.847728f,   0.839883f,   0.832098f,
    0.824375f,   0.816712f,   0.809108f,   0.801564f,   0.794079f,   0.786653f,   0.779284f,
    0.771974f,   0.76472f,    0.757523f,   0.750382f,   0.743297f,   0.736267f,   0.729292f,
    0.722372f,   0.715505f,   0.708693f,   0.701933f,   0.695227f,   0.688572f,   0.68197f,
    0.67542f,    0.66892f,    0.662471f,   0.656073f,   0.649725f,   0.643426f,   0.637176f,
    0.630976f,   0.624824f,   0.618719f,   0.612663f,   0.606654f,   0.600691f,   0.594776f,
    0.588906f,   0.583083f,   0.577305f,   0.571572f,   0.565883f,   0.56024f,    0.55464f,
    0.549084f,   0.543572f,   0.538102f,   0.532676f,   0.527291f,   0.521949f,   0.516649f,
    0.511389f,   0.506171f,   0.500994f,   0.495857f,   0.490761f,   0.485704f,   0.480687f,
    0.475709f,   0.470769f,   0.465869f,   0.461006f,   0.456182f,   0.451395f,   0.446646f,
    0.441934f,   0.437258f,   0.432619f,   0.428017f,   0.42345f,    0.418919f,   0.414424f,
    0.409963f,   0.405538f,   0.401147f,   0.39679f,    0.392467f,   0.388178f,   0.383923f,
    0.379701f,   0.375511f,   0.371355f,   0.367231f,   0.363139f,   0.359079f,   0.355051f,
    0.351055f,   0.347089f,   0.343155f,   0.339251f,   0.335378f,   0.331535f,   0.327722f,
    0.323939f,   0.320186f,   0.316461f,   0.312766f,   0.3091f,     0.305462f,   0.301853f,
    0.298272f,   0.294719f,   0.291194f,   0.287696f,   0.284226f,   0.280782f,   0.277366f,
    0.273976f,   0.270613f,   0.267276f,   0.263965f,   0.26068f,    0.257421f,   0.254187f,
    0.250979f,   0.247795f,   0.244636f,   0.241502f,   0.238393f,   0.235308f,   0.232246f,
    0.229209f,   0.226196f,   0.223206f,   0.220239f,   0.217296f,   0.214375f,   0.211478f,
    0.208603f,   0.20575f,    0.20292f,    0.200112f,   0.197326f,   0.194562f,   0.191819f,
    0.189097f,   0.186397f,   0.183718f,   0.18106f,    0.178423f,   0.175806f,   0.17321f,
    0.170634f,   0.168078f,   0.165542f,   0.163026f,   0.16053f,    0.158053f,   0.155595f,
    0.153157f,   0.150738f,   0.148337f,   0.145955f,   0.143592f,   0.141248f,   0.138921f,
    0.136613f,   0.134323f,   0.132051f,   0.129797f,   0.12756f,    0.125341f,   0.123139f,
    0.120954f,   0.118786f,   0.116635f,   0.114501f,   0.112384f,   0.110283f,   0.108199f,
    0.106131f,   0.104079f,   0.102043f,   0.100023f,   0.0980186f,  0.09603f,    0.094057f,
    0.0920994f,  0.0901571f,  0.08823f,    0.0863179f,  0.0844208f,  0.0825384f,  0.0806708f,
    0.0788178f,  0.0769792f,  0.0751551f,  0.0733451f,  0.0715493f,  0.0697676f,  0.0679997f,
    0.0662457f,  0.0645054f,  0.0627786f,  0.0610654f,  0.0593655f,  0.0576789f,  0.0560055f,
    0.0543452f,  0.0526979f,  0.0510634f,  0.0494416f,  0.0478326f,  0.0462361f,  0.0446521f,
    0.0430805f,  0.0415211f,  0.039974f,   0.0384389f,  0.0369158f,  0.0354046f,  0.0339052f,
    0.0324175f,  0.0309415f,  0.029477f,   0.0280239f,  0.0265822f,  0.0251517f,  0.0237324f,
    0.0223242f,  0.020927f,   0.0195408f,  0.0181653f,  0.0168006f,  0.0154466f,  0.0141031f,
    0.0127701f,  0.0114476f,  0.0101354f,  0.00883339f, 0.00754159f, 0.00625989f, 0.00498819f,
    0.00372644f, 0.00247454f, 0.00123242f, 0.0f,
};

static void radangle2imp(float a2, float b2, float th, float *A, float *B, float *C, float *F)
{
  float ct2 = cosf(th);
  const float st2 = 1.0f - ct2 * ct2; /* <- sin(th)^2 */
  ct2 *= ct2;
  *A = a2 * st2 + b2 * ct2;
  *B = (b2 - a2) * sinf(2.0f * th);
  *C = a2 * ct2 + b2 * st2;
  *F = a2 * b2;
}

void BLI_ewa_imp2radangle(
    float A, float B, float C, float F, float *a, float *b, float *th, float *ecc)
{
  /* NOTE: all tests here are done to make sure possible overflows are hopefully minimized. */

  if (F <= 1e-5f) { /* use arbitrary major radius, zero minor, infinite eccentricity */
    *a = sqrtf(A > C ? A : C);
    *b = 0.0f;
    *ecc = 1e10f;
    *th = 0.5f * (atan2f(B, A - C) + float(M_PI));
  }
  else {
    const float AmC = A - C, ApC = A + C, F2 = F * 2.0f;
    const float r = sqrtf(AmC * AmC + B * B);
    float d = ApC - r;
    *a = (d <= 0.0f) ? sqrtf(A > C ? A : C) : sqrtf(F2 / d);
    d = ApC + r;
    if (d <= 0.0f) {
      *b = 0.0f;
      *ecc = 1e10f;
    }
    else {
      *b = sqrtf(F2 / d);
      *ecc = *a / *b;
    }
    /* Increase theta by `0.5 * pi` (angle of major axis). */
    *th = 0.5f * (atan2f(B, AmC) + float(M_PI));
  }
}

void BLI_ewa_filter(const int width,
                    const int height,
                    const bool intpol,
                    const bool use_alpha,
                    const float uv[2],
                    const float du[2],
                    const float dv[2],
                    ewa_filter_read_pixel_cb read_pixel_cb,
                    void *userdata,
                    float result[4])
{
  /* Scaling `dxt` / `dyt` by full resolution can cause overflow because of huge A/B/C and esp.
   * F values, scaling by aspect ratio alone does the opposite, so try something in between
   * instead. */
  const float ff2 = float(width), ff = sqrtf(ff2), q = float(height) / ff;
  const float Ux = du[0] * ff, Vx = du[1] * q, Uy = dv[0] * ff, Vy = dv[1] * q;
  float A = Vx * Vx + Vy * Vy;
  float B = -2.0f * (Ux * Vx + Uy * Vy);
  float C = Ux * Ux + Uy * Uy;
  float F = A * C - B * B * 0.25f;
  float a, b, th, ecc, a2, b2, ue, ve, U0, V0, DDQ, U, ac1, ac2, BU, d;
  int u, v, u1, u2, v1, v2;

  /* The so-called 'high' quality EWA method simply adds a constant of 1 to both A & C,
   * so the ellipse always covers at least some texels. But since the filter is now always larger,
   * it also means that everywhere else it's also more blurry then ideally should be the case.
   * So instead here the ellipse radii are modified instead whenever either is too low.
   * Use a different radius based on interpolation switch,
   * just enough to anti-alias when interpolation is off,
   * and slightly larger to make result a bit smoother than bilinear interpolation when
   * interpolation is on (minimum values: `const float rmin = intpol ? 1.0f : 0.5f;`) */
  const float rmin = (intpol ? 1.5625f : 0.765625f) / ff2;
  BLI_ewa_imp2radangle(A, B, C, F, &a, &b, &th, &ecc);
  if ((b2 = b * b) < rmin) {
    if ((a2 = a * a) < rmin) {
      UNUSED_VARS(a2, b2);
      B = 0.0f;
      A = C = rmin;
      F = A * C;
    }
    else {
      b2 = rmin;
      radangle2imp(a2, b2, th, &A, &B, &C, &F);
    }
  }

  ue = ff * sqrtf(C);
  ve = ff * sqrtf(A);
  d = float(EWA_MAXIDX + 1) / (F * ff2);
  A *= d;
  B *= d;
  C *= d;

  U0 = uv[0] * float(width);
  V0 = uv[1] * float(height);
  u1 = int(floorf(U0 - ue));
  u2 = int(ceilf(U0 + ue));
  v1 = int(floorf(V0 - ve));
  v2 = int(ceilf(V0 + ve));

  /* sane clamping to avoid unnecessarily huge loops */
  /* NOTE: if eccentricity gets clamped (see above),
   * the ue/ve limits can also be lowered accordingly
   */
  if (U0 - float(u1) > EWA_MAXIDX) {
    u1 = int(U0) - EWA_MAXIDX;
  }
  if (float(u2) - U0 > EWA_MAXIDX) {
    u2 = int(U0) + EWA_MAXIDX;
  }
  if (V0 - float(v1) > EWA_MAXIDX) {
    v1 = int(V0) - EWA_MAXIDX;
  }
  if (float(v2) - V0 > EWA_MAXIDX) {
    v2 = int(V0) + EWA_MAXIDX;
  }

  /* Early output check for cases the whole region is outside of the buffer. */
  if ((u2 < 0 || u1 >= width) || (v2 < 0 || v1 >= height)) {
    zero_v4(result);
    return;
  }

  U0 -= 0.5f;
  V0 -= 0.5f;
  DDQ = 2.0f * A;
  U = float(u1) - U0;
  ac1 = A * (2.0f * U + 1.0f);
  ac2 = A * U * U;
  BU = B * U;

  d = 0.0f;
  zero_v4(result);
  for (v = v1; v <= v2; v++) {
    const float V = float(v) - V0;
    float DQ = ac1 + B * V;
    float Q = (C * V + BU) * V + ac2;
    for (u = u1; u <= u2; u++) {
      if (Q < float(EWA_MAXIDX + 1)) {
        float tc[4];
        const float wt = EWA_WTS[(Q < 0.0f) ? 0 : uint(Q)];
        read_pixel_cb(userdata, u, v, tc);
        madd_v3_v3fl(result, tc, wt);
        result[3] += use_alpha ? tc[3] * wt : 0.0f;
        d += wt;
      }
      Q += DQ;
      DQ += DDQ;
    }
  }

  /* `d` should hopefully never be zero anymore. */
  d = 1.0f / d;
  mul_v3_fl(result, d);
  /* Clipping can be ignored if alpha used, `texr->trgba[3]` already includes filtered edge. */
  result[3] = use_alpha ? result[3] * d : 1.0f;
}

/* INFO: bigger α drops weight faster → less smoothing.
Precomputed Gaussian LUT: w(r^2) = exp(-alpha * r^2), r^2 sampled at i/512, i=0..511
Auto-generated Gaussian LUT for EWA: w(r^2) = exp(-alpha * r^2) */
static constexpr int EWA_LUT_SIZE = 512;
static constexpr int EWA_GAUSS_MAXIDX = (EWA_LUT_SIZE - 1);

static const float EWA_GAUSS_LUT[EWA_LUT_SIZE] = {
    1.000000f, 0.996094f, 0.992203f, 0.988327f, 0.984466f, 0.980621f, 0.976790f, 0.972975f,
    0.969174f, 0.965388f, 0.961617f, 0.957861f, 0.954119f, 0.950392f, 0.946680f, 0.942982f,
    0.939298f, 0.935629f, 0.931974f, 0.928334f, 0.924707f, 0.921095f, 0.917497f, 0.913913f,
    0.910343f, 0.906787f, 0.903245f, 0.899717f, 0.896202f, 0.892702f, 0.889214f, 0.885741f,
    0.882281f, 0.878835f, 0.875402f, 0.871982f, 0.868576f, 0.865183f, 0.861804f, 0.858437f,
    0.855084f, 0.851744f, 0.848417f, 0.845102f, 0.841801f, 0.838513f, 0.835238f, 0.831975f,
    0.828725f, 0.825488f, 0.822263f, 0.819051f, 0.815852f, 0.812665f, 0.809490f, 0.806328f,
    0.803179f, 0.800041f, 0.796916f, 0.793803f, 0.790702f, 0.787614f, 0.784537f, 0.781472f,
    0.778420f, 0.775379f, 0.772350f, 0.769333f, 0.766328f, 0.763335f, 0.760353f, 0.757383f,
    0.754424f, 0.751477f, 0.748542f, 0.745618f, 0.742705f, 0.739804f, 0.736914f, 0.734036f,
    0.731168f, 0.728312f, 0.725467f, 0.722633f, 0.719811f, 0.716999f, 0.714198f, 0.711408f,
    0.708629f, 0.705861f, 0.703104f, 0.700357f, 0.697622f, 0.694897f, 0.692182f, 0.689478f,
    0.686785f, 0.684102f, 0.681430f, 0.678768f, 0.676117f, 0.673476f, 0.670845f, 0.668224f,
    0.665614f, 0.663014f, 0.660424f, 0.657845f, 0.655275f, 0.652715f, 0.650165f, 0.647626f,
    0.645096f, 0.642576f, 0.640066f, 0.637566f, 0.635075f, 0.632595f, 0.630123f, 0.627662f,
    0.625210f, 0.622768f, 0.620335f, 0.617912f, 0.615498f, 0.613094f, 0.610699f, 0.608314f,
    0.605937f, 0.603571f, 0.601213f, 0.598864f, 0.596525f, 0.594195f, 0.591874f, 0.589562f,
    0.587259f, 0.584965f, 0.582680f, 0.580404f, 0.578137f, 0.575878f, 0.573629f, 0.571388f,
    0.569156f, 0.566933f, 0.564718f, 0.562512f, 0.560315f, 0.558126f, 0.555946f, 0.553774f,
    0.551611f, 0.549456f, 0.547310f, 0.545172f, 0.543043f, 0.540921f, 0.538808f, 0.536704f,
    0.534607f, 0.532519f, 0.530439f, 0.528367f, 0.526303f, 0.524247f, 0.522199f, 0.520159f,
    0.518127f, 0.516103f, 0.514087f, 0.512079f, 0.510079f, 0.508086f, 0.506102f, 0.504125f,
    0.502156f, 0.500194f, 0.498240f, 0.496294f, 0.494355f, 0.492424f, 0.490501f, 0.488585f,
    0.486676f, 0.484775f, 0.482881f, 0.480995f, 0.479116f, 0.477245f, 0.475380f, 0.473523f,
    0.471674f, 0.469831f, 0.467996f, 0.466168f, 0.464347f, 0.462533f, 0.460726f, 0.458927f,
    0.457134f, 0.455348f, 0.453570f, 0.451798f, 0.450033f, 0.448275f, 0.446524f, 0.444780f,
    0.443042f, 0.441312f, 0.439588f, 0.437871f, 0.436160f, 0.434456f, 0.432759f, 0.431069f,
    0.429385f, 0.427708f, 0.426037f, 0.424373f, 0.422715f, 0.421064f, 0.419419f, 0.417781f,
    0.416149f, 0.414523f, 0.412904f, 0.411291f, 0.409685f, 0.408084f, 0.406490f, 0.404902f,
    0.403321f, 0.401745f, 0.400176f, 0.398613f, 0.397056f, 0.395505f, 0.393960f, 0.392421f,
    0.390888f, 0.389361f, 0.387840f, 0.386325f, 0.384816f, 0.383313f, 0.381815f, 0.380324f,
    0.378838f, 0.377358f, 0.375884f, 0.374416f, 0.372954f, 0.371497f, 0.370046f, 0.368600f,
    0.367160f, 0.365726f, 0.364297f, 0.362874f, 0.361457f, 0.360045f, 0.358639f, 0.357238f,
    0.355842f, 0.354452f, 0.353068f, 0.351688f, 0.350315f, 0.348946f, 0.347583f, 0.346225f,
    0.344873f, 0.343526f, 0.342184f, 0.340847f, 0.339516f, 0.338190f, 0.336869f, 0.335553f,
    0.334242f, 0.332936f, 0.331636f, 0.330340f, 0.329050f, 0.327765f, 0.326484f, 0.325209f,
    0.323939f, 0.322673f, 0.321413f, 0.320157f, 0.318907f, 0.317661f, 0.316420f, 0.315184f,
    0.313953f, 0.312726f, 0.311505f, 0.310288f, 0.309076f, 0.307869f, 0.306666f, 0.305468f,
    0.304275f, 0.303086f, 0.301902f, 0.300723f, 0.299548f, 0.298378f, 0.297213f, 0.296052f,
    0.294895f, 0.293743f, 0.292596f, 0.291453f, 0.290314f, 0.289180f, 0.288051f, 0.286926f,
    0.285805f, 0.284688f, 0.283576f, 0.282469f, 0.281365f, 0.280266f, 0.279171f, 0.278081f,
    0.276995f, 0.275913f, 0.274835f, 0.273761f, 0.272692f, 0.271627f, 0.270566f, 0.269509f,
    0.268456f, 0.267407f, 0.266363f, 0.265322f, 0.264286f, 0.263253f, 0.262225f, 0.261201f,
    0.260181f, 0.259164f, 0.258152f, 0.257143f, 0.256139f, 0.255138f, 0.254142f, 0.253149f,
    0.252160f, 0.251175f, 0.250194f, 0.249217f, 0.248243f, 0.247273f, 0.246308f, 0.245345f,
    0.244387f, 0.243432f, 0.242482f, 0.241534f, 0.240591f, 0.239651f, 0.238715f, 0.237782f,
    0.236854f, 0.235928f, 0.235007f, 0.234089f, 0.233174f, 0.232264f, 0.231356f, 0.230453f,
    0.229552f, 0.228656f, 0.227762f, 0.226873f, 0.225987f, 0.225104f, 0.224224f, 0.223349f,
    0.222476f, 0.221607f, 0.220741f, 0.219879f, 0.219020f, 0.218165f, 0.217313f, 0.216464f,
    0.215618f, 0.214776f, 0.213937f, 0.213101f, 0.212269f, 0.211440f, 0.210614f, 0.209791f,
    0.208971f, 0.208155f, 0.207342f, 0.206532f, 0.205725f, 0.204922f, 0.204121f, 0.203324f,
    0.202530f, 0.201739f, 0.200950f, 0.200166f, 0.199384f, 0.198605f, 0.197829f, 0.197056f,
    0.196286f, 0.195520f, 0.194756f, 0.193995f, 0.193237f, 0.192483f, 0.191731f, 0.190982f,
    0.190236f, 0.189493f, 0.188752f, 0.188015f, 0.187281f, 0.186549f, 0.185820f, 0.185095f,
    0.184372f, 0.183651f, 0.182934f, 0.182219f, 0.181508f, 0.180799f, 0.180092f, 0.179389f,
    0.178688f, 0.177990f, 0.177295f, 0.176602f, 0.175912f, 0.175225f, 0.174541f, 0.173859f,
    0.173180f, 0.172503f, 0.171830f, 0.171158f, 0.170490f, 0.169824f, 0.169160f, 0.168500f,
    0.167841f, 0.167186f, 0.166533f, 0.165882f, 0.165234f, 0.164589f, 0.163946f, 0.163305f,
    0.162668f, 0.162032f, 0.161399f, 0.160769f, 0.160141f, 0.159515f, 0.158892f, 0.158271f,
    0.157653f, 0.157037f, 0.156424f, 0.155813f, 0.155204f, 0.154598f, 0.153994f, 0.153392f,
    0.152793f, 0.152196f, 0.151602f, 0.151010f, 0.150420f, 0.149832f, 0.149247f, 0.148664f,
    0.148083f, 0.147505f, 0.146929f, 0.146355f, 0.145783f, 0.145214f, 0.144646f, 0.144081f,
    0.143518f, 0.142958f, 0.142399f, 0.141843f, 0.141289f, 0.140737f, 0.140187f, 0.139640f,
    0.139094f, 0.138551f, 0.138010f, 0.137471f, 0.136934f, 0.136399f, 0.135866f, 0.135335f};

/* Linear-interpolated Gaussian weight from LUT (r2 in [0,1]). */
static inline float ewa_weight(float r2, bool interpolate = true)
{
  if (r2 <= 0.0f) {
    return EWA_GAUSS_LUT[0];
  }
  if (r2 >= 1.0f) {
    return EWA_GAUSS_LUT[EWA_GAUSS_MAXIDX];
  }
  float t = r2 * static_cast<float>(EWA_GAUSS_MAXIDX);
  int i = int(t);

  if (interpolate) {
    float f = t - float(i);
    int i1 = (i < EWA_GAUSS_MAXIDX) ? (i + 1) : EWA_GAUSS_MAXIDX;
    return EWA_GAUSS_LUT[i] * (1.0f - f) + EWA_GAUSS_LUT[i1] * f;
  }

  return EWA_GAUSS_LUT[i];
}

/* PBRT-style anisotropy clamp on the derivative vectors in *texel* space. */
static inline void aniso_clamp(float &dx0, float &dy0, float &dx1, float &dy1, float maxAniso)
{
  auto len2 = [](float x, float y) { return x * x + y * y; };
  // Make (dx0,dy0) the longer axis
  if (len2(dx0, dy0) < len2(dx1, dy1)) {
    std::swap(dx0, dx1);
    std::swap(dy0, dy1);
  }
  float longer = std::sqrt(std::max(0.f, len2(dx0, dy0)));
  float shorter = std::sqrt(std::max(0.f, len2(dx1, dy1)));
  if (shorter > 0.f && longer > maxAniso * shorter) {
    float scale = longer / (maxAniso * shorter);
    dx1 *= scale;
    dy1 *= scale;
  }
}

/* Build normalized PBRT ellipse coefficients (A,B,C) and bbox in texel space. */
struct Ellipse {
  // normalized quadratic r^2 = A*ss^2 + B*ss*tt + C*tt^2
  float A, B, C;
  // bbox of integer texels to scan
  int s0, s1, t0, t1;
  // center in texel space
  float stx, sty;
  bool valid = false;
};

static inline Ellipse build_ellipse(int width,
                                    int height,
                                    float s,
                                    float t,
                                    const float du[2],
                                    const float dv[2],
                                    float maxAniso = 8.0f)
{
  float dx0 = du[0] * width;
  float dy0 = du[1] * height;
  float dx1 = dv[0] * width;
  float dy1 = dv[1] * height;

  if (maxAniso > 1.0f) {
    aniso_clamp(dx0, dy0, dx1, dy1, maxAniso);
  }

  float A = dy0 * dy0 + dy1 * dy1 + 1.0f;
  float B = -2.0f * (dx0 * dy0 + dx1 * dy1);
  float C = dx0 * dx0 + dx1 * dx1 + 1.0f;
  float invF = 1.0f / (A * C - 0.25f * B * B);
  A *= invF;
  B *= invF;
  C *= invF;

  // Center in texel space (texel centers at i+0.5)
  float stx = s * width - 0.5f;
  float sty = t * height - 0.5f;

  // bbox
  float det = -B * B + 4.0f * A * C;
  if (!(det > 0.0f)) {
    return Ellipse{};  // invalid
  }
  float invDet = 1.0f / det;
  float uSqrt = std::sqrt(std::max(0.0f, det * C));
  float vSqrt = std::sqrt(std::max(0.0f, A * det));

  int s0 = int(std::ceil(stx - 2.0f * invDet * uSqrt));
  int s1 = int(std::floor(stx + 2.0f * invDet * uSqrt));
  int t0 = int(std::ceil(sty - 2.0f * invDet * vSqrt));
  int t1 = int(std::floor(sty + 2.0f * invDet * vSqrt));
  return {A, B, C, s0, s1, t0, t1, stx, sty, true};
}

/* -----------------------------------------------------------------------------
// PBRT-style single-level EWA filtering (no mipmaps)
//
// Assumptions:
//   * uv in [0,1]^2
//   * du = d(uv)/dx, dv = d(uv)/dy
//   * ewa_weight(r2, interpolate=true) returns Gaussian LUT weight in [0,1],
//     e.g. w = exp(-alpha * r^2), with r^2 in [0,1].
//   * Read callback returns RGBA (alpha optional).
// ----------------------------------------------------------------------------- */
void BLI_ewa_single_level(const int width,
                          const int height,
                          const bool use_alpha,
                          const float uv[2],
                          const float du[2],
                          const float dv[2],
                          ewa_filter_read_pixel_cb read_pixel_cb,
                          void *userdata,
                          float result[4],
                          float maxAniso,
                          bool interpolate_lut)
{
  Ellipse E = build_ellipse(width, height, uv[0], uv[1], du, dv, maxAniso);
  if (!E.valid) {
    result[0] = result[1] = result[2] = 0.0f;
    result[3] = use_alpha ? 0.0f : 1.0f;
    return;
  }

  // Clip bbox to image so callback isn't called OOB (adjust to your addressing mode)
  int s0 = std::max(E.s0, 0), s1 = std::min(E.s1, width - 1);
  int t0 = std::max(E.t0, 0), t1 = std::min(E.t1, height - 1);
  if (s0 > s1 || t0 > t1) {
    result[0] = result[1] = result[2] = 0.0f;
    result[3] = use_alpha ? 0.0f : 1.0f;
    return;
  }

  float accum[4] = {0, 0, 0, 0};
  float wsum = 0.0f;

  // Incremental evaluation along x: r2_next = r2 + A*(2*ss+1) + B*tt
  for (int y = t0; y <= t1; ++y) {
    float tt = float(y) - E.sty;

    float ss0 = float(s0) - E.stx;
    float r2 = E.A * ss0 * ss0 + E.B * ss0 * tt + E.C * tt * tt;
    float dR = E.A * (2.0f * ss0 + 1.0f) + E.B * tt;
    const float ddR = 2.0f * E.A;

    for (int x = s0; x <= s1; ++x) {
      if (r2 < 1.0f) {
        float wt = ewa_weight(r2, interpolate_lut);
        if (wt > 0.0f) {
          float rgba[4];
          read_pixel_cb(userdata, x, y, rgba);
          accum[0] += wt * rgba[0];
          accum[1] += wt * rgba[1];
          accum[2] += wt * rgba[2];
          if (use_alpha) {
            accum[3] += wt * rgba[3];
          }
          wsum += wt;
        }
      }
      r2 += dR;
      dR += ddR;
    }
  }

  if (wsum > 0.0f) {
    float inv = 1.0f / wsum;
    result[0] = accum[0] * inv;
    result[1] = accum[1] * inv;
    result[2] = accum[2] * inv;
    result[3] = use_alpha ? (accum[3] * inv) : 1.0f;
  }
  else {
    result[0] = result[1] = result[2] = 0.0f;
    result[3] = use_alpha ? 0.0f : 1.0f;
  }
}
