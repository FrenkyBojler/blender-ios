/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "gpu_shader_math_base_lib.glsl"
#include "gpu_glsl_cpp_stubs.hh"

// TODO: Pass as 2D texture
#define EWA_LUT_SIZE     256
#define EWA_GAUSS_MAXIDX (EWA_LUT_SIZE - 1)

const float EWA_GAUSS_LUT[EWA_LUT_SIZE] = float_array(
  1.000000f, 0.992188f, 0.984436f, 0.976745f, 0.969114f, 0.961543f, 0.954031f, 0.946578f,
  0.939183f, 0.931846f, 0.924566f, 0.917342f, 0.910176f, 0.903065f, 0.896010f, 0.889010f,
  0.882064f, 0.875173f, 0.868336f, 0.861552f, 0.854821f, 0.848143f, 0.841517f, 0.834943f,
  0.828420f, 0.821948f, 0.815526f, 0.809155f, 0.802834f, 0.796561f, 0.790338f, 0.784164f,
  0.778038f, 0.771959f, 0.765928f, 0.759945f, 0.754008f, 0.748117f, 0.742272f, 0.736473f,
  0.730720f, 0.725011f, 0.719347f, 0.713727f, 0.708151f, 0.702619f, 0.697129f, 0.691683f,
  0.686279f, 0.680918f, 0.675598f, 0.670320f, 0.665083f, 0.659887f, 0.654732f, 0.649617f,
  0.644542f, 0.639506f, 0.634510f, 0.629553f, 0.624635f, 0.619755f, 0.614913f, 0.610109f,
  0.605343f, 0.600613f, 0.595921f, 0.591265f, 0.586646f, 0.582063f, 0.577516f, 0.573004f,
  0.568527f, 0.564086f, 0.559679f, 0.555306f, 0.550968f, 0.546664f, 0.542393f, 0.538155f,
  0.533951f, 0.529780f, 0.525641f, 0.521534f, 0.517460f, 0.513417f, 0.509406f, 0.505426f,
  0.501478f, 0.497560f, 0.493673f, 0.489816f, 0.485989f, 0.482193f, 0.478425f, 0.474688f,
  0.470979f, 0.467300f, 0.463649f, 0.460027f, 0.456433f, 0.452867f, 0.449329f, 0.445819f,
  0.442336f, 0.438880f, 0.435451f, 0.432049f, 0.428674f, 0.425325f, 0.422002f, 0.418705f,
  0.415434f, 0.412189f, 0.408968f, 0.405773f, 0.402603f, 0.399458f, 0.396337f, 0.393241f,
  0.390169f, 0.387120f, 0.384096f, 0.381095f, 0.378118f, 0.375164f, 0.372233f, 0.369325f,
  0.366440f, 0.363577f, 0.360736f, 0.357918f, 0.355122f, 0.352348f, 0.349595f, 0.346864f,
  0.344154f, 0.341465f, 0.338797f, 0.336151f, 0.333524f, 0.330919f, 0.328333f, 0.325768f,
  0.323223f, 0.320698f, 0.318193f, 0.315707f, 0.313240f, 0.310793f, 0.308365f, 0.305956f,
  0.303566f, 0.301194f, 0.298841f, 0.296506f, 0.294190f, 0.291892f, 0.289611f, 0.287349f,
  0.285104f, 0.282876f, 0.280666f, 0.278474f, 0.276298f, 0.274140f, 0.271998f, 0.269873f,
  0.267765f, 0.265673f, 0.263597f, 0.261538f, 0.259495f, 0.257467f, 0.255456f, 0.253460f,
  0.251480f, 0.249515f, 0.247566f, 0.245632f, 0.243713f, 0.241809f, 0.239920f, 0.238045f,
  0.236186f, 0.234340f, 0.232510f, 0.230693f, 0.228891f, 0.227103f, 0.225328f, 0.223568f,
  0.221821f, 0.220089f, 0.218369f, 0.216663f, 0.214970f, 0.213291f, 0.211625f, 0.209971f,
  0.208331f, 0.206703f, 0.205088f, 0.203486f, 0.201897f, 0.200319f, 0.198754f, 0.197201f,
  0.195661f, 0.194132f, 0.192616f, 0.191111f, 0.189618f, 0.188136f, 0.186667f, 0.185208f,
  0.183761f, 0.182273f, 0.180585f, 0.178551f, 0.176063f, 0.173043f, 0.169441f, 0.165229f,
  0.160403f, 0.154976f, 0.148978f, 0.142453f, 0.135454f, 0.128045f, 0.120298f, 0.112287f,
  0.104092f, 0.095795f, 0.087478f, 0.079221f, 0.071103f, 0.063199f, 0.055579f, 0.048310f,
  0.041449f, 0.035048f, 0.029150f, 0.023791f, 0.018994f, 0.014776f, 0.011143f, 0.008087f,
  0.005594f, 0.003634f, 0.002167f, 0.001143f, 0.000496f, 0.000151f, 0.000019f, 0.000000f);


void clamp_anisotropy(inout float2 du_dx_texels,
                      inout float2 du_dy_texels,
                      float max_ratio_between_axes)
{
  /* Make du/dx the longer axis; clamp the shorter (du/dy) if needed. */
  float len_squared_dx = dot(du_dx_texels, du_dx_texels);
  float len_squared_dy = dot(du_dy_texels, du_dy_texels);
  if (len_squared_dx < len_squared_dy) {
    /* Swap */
    float2 tmp = du_dx_texels;
    du_dx_texels = du_dy_texels;
    du_dy_texels = tmp;
    float tmpf = len_squared_dx;
    len_squared_dx = len_squared_dy;
    len_squared_dy = tmpf;
  }

  float long_len  = sqrt(len_squared_dx);
  float short_len = sqrt(len_squared_dy);

  if (max_ratio_between_axes > 1.0 && short_len > 0.0 && long_len > max_ratio_between_axes *
    short_len) {
    float scale = long_len / (max_ratio_between_axes * short_len);
    du_dy_texels *= scale;
  }
}

float lookup_ewa_weight(float r2_normalized)
{
  /* Clamp the query range explicitly; the LUT is only defined on [0,1]. */
  if (r2_normalized <= 0.0f) {
    return EWA_GAUSS_LUT[0];
  }
  if (r2_normalized >= 1.0f) {
    return EWA_GAUSS_LUT[EWA_GAUSS_MAXIDX];
  }

  /* Convert r^2 ∈ [0,1) to fractional LUT index t ∈ [0, EWA_GAUSS_MAXIDX). */
  float t_float = r2_normalized * float(EWA_GAUSS_MAXIDX);
  int bin_index = int(t_float);  // floor

  /* Linear interpolation between bin_index and bin_index+1. */
  float frac = t_float - float(bin_index);
  int next_i = bin_index < EWA_GAUSS_MAXIDX ? bin_index + 1 : EWA_GAUSS_MAXIDX;

  float w0 = EWA_GAUSS_LUT[bin_index];
  float w1 = EWA_GAUSS_LUT[next_i];
  return w0 * (1.0f - frac) + w1 * frac;
}

float compute_gaussian_weight(float r2_normalized, float alpha) {
  return exp(-alpha * r2_normalized);
}

float smootherstep01(float x)
{
  float t = clamp(x, 0.0, 1.0);
  return ((6.0*t - 15.0)*t + 10.0) * t*t*t;
}

/* Optional edge fade if LUT is unwindowed (keeps interior identical)
 Fade starts at r_soft (≈0.92-0.95) and goes to 0 with zero slope at r=1. */
float apply_edge_fade(float w_lut, float r2, bool usePreWindowedLUT)
{
  if (usePreWindowedLUT) {
    return w_lut;
  }
  if (r2 >= 1.0f) {
    return 0.0f;
  }

  /* begin fading in last ~8% of radius */
  const float r_soft = 0.92f;
  float r2_soft = r_soft * r_soft;
  if (r2 <= r2_soft) {
    return w_lut;
  }

  float r = sqrt(r2);
  float t = (r - r_soft) / (1.0 - r_soft);
  float fade = 1.0 - smootherstep01(t);
  return w_lut * fade;
}

struct Ellipse {
  float A, B, C;
  float center_u_texel, center_v_texel;
  int s0, s1, t0, t1;
  bool valid;
};

Ellipse build_ellipse(float2 uv_center_norm, float2 du_dx_texels, float2 du_dy_texels, float max_ratio_between_axes)
{
  /* Clamps the ellipsoid based on the ratio between the axes.
  This leads to better performance, because thin ellipsoids will be adjusted,
  but also leads to neglectable blurring. */
  if (max_ratio_between_axes > 1.0) {
    clamp_anisotropy(du_dx_texels, du_dy_texels, max_ratio_between_axes);
  }


  float dv_dx = du_dx_texels.y;
  float dv_dy = du_dy_texels.y;
  float du_dx = du_dx_texels.x;
  float du_dy = du_dy_texels.x;

  float A = dv_dx*dv_dx + dv_dy*dv_dy + 1.0;
  float B = -2.0 * (du_dx*dv_dx + du_dy*dv_dy);
  float C = du_dx*du_dx + du_dy*du_dy + 1.0;

  float denom = A * C - 0.25 * B * B;
  if (!(denom > 0.0)) {
    Ellipse e;
    e.valid = false;
    return e;
  }

  float inv = 1.0 / denom;
  A *= inv;
  B *= inv;
  C *= inv;

  float center_u_texel = uv_center_norm.x - 0.5;
  float center_v_texel = uv_center_norm.y - 0.5;

  float conic_discriminant = -B * B + 4.0 * A * C;
  if (conic_discriminant < 0.0) {
    Ellipse e;
    e.valid = false;
    return e;
  }

  float inv_conic_discriminant = 1.0 / conic_discriminant;
  float u_extent = 2.0 * inv_conic_discriminant * sqrt(conic_discriminant * C);
  float v_extent = 2.0 * inv_conic_discriminant * sqrt(A * conic_discriminant);

  int s0 = int(ceil (center_u_texel - u_extent));
  int s1 = int(floor(center_u_texel + u_extent));
  int t0 = int(ceil (center_v_texel - v_extent));
  int t1 = int(floor(center_v_texel + v_extent));

  Ellipse e;
  e.A = A;
  e.B = B;
  e.C = C;
  e.center_u_texel = center_u_texel;
  e.center_v_texel = center_v_texel;
  e.s0 = s0;
  e.s1 = s1;
  e.t0 = t0;
  e.t1 = t1;
  e.valid = (s0 <= s1 && t0 <= t1);
  return e;
}

float4 texture_ewa(sampler2D input_tx, float2 coordinates, float2 x_gradient, float2 y_gradient
                   /*should be pushed as a constant? float max_ratio_between_axes = 8.0f*/)
{
  float max_ratio_between_axes = 8.0f;

  /* Bring derivatives to texel space */
  float2 size = float2(textureSize(input_tx, 0));

  float2 uv_center_norm = float2(coordinates.x * size.x, coordinates.y * size.y);
  float2 du_dx_texels = float2(x_gradient.x * size.x, x_gradient.y * size.y);
  float2 du_dy_texels = float2(y_gradient.x * size.x, y_gradient.y * size.y);
  // float2 du_dx_texels = float2(du_dx_norm.x * float(size.x),
  //                              du_dx_norm.y * float(size.y));
  // float2 du_dy_texels = float2(du_dy_norm.x * float(size.x),
  //                              du_dy_norm.y * float(size.y));

  /* Build ellipse & bbox in texel space */
  Ellipse e = build_ellipse(uv_center_norm, du_dx_texels, du_dy_texels, max_ratio_between_axes);

  if (!e.valid) {
    return float4(0.0f);
  }

  float4 accum = float4(0.0f);
  float wsum = 0.0f;

  /* Incremental evaluation across each scanline */
  for (int y = e.t0; y <= e.t1; ++y) {
    float delta_v = float(y) - e.center_v_texel;

    float delta_u0 = float(e.s0) - e.center_u_texel;
    float r2 = e.A * delta_u0 * delta_u0 + e.B * delta_u0 * delta_v + e.C * delta_v * delta_v;

    float dR = e.A * (2.0f * delta_u0 + 1.0f) + e.B * delta_v;
    float ddR = 2.0f * e.A;

    for (int x = e.s0; x <= e.s1; ++x) {
      if (r2 < 1.0f) {
        /* INFO: we can implement different version with different fades which would
        affect the discontinuities differently.
        float w = apply_edge_fade(w_lut, r2, uUsePreWindowedLUT); */
        float weight = lookup_ewa_weight(r2);
        //float weight = compute_gaussian_weight(r2, 2.0f);
        if (weight > 0.0f) {
          float4 rgba = texelFetch(input_tx, int2(x, y), 0);
          accum += weight * rgba;
          wsum  += weight;
        }
      }
      r2 += dR;
      dR += ddR;
    }
  }

  float4 out_color = (wsum > 0.0f) ? (accum / wsum) : float4(0.0f);
  return out_color;
}
