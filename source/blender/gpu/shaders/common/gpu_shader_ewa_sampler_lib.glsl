/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "gpu_shader_math_base_lib.glsl"
#include "gpu_glsl_cpp_stubs.hh"

/* Clamp anisotropy of the footprint: ensure the short axis is not more
 * than the `max_ratio_between_axes` times shorter than the long axis. Inputs are the
 * two derivative vectors in texel space (du/dx, dv/dx) and (du/dy, dv/dy). */
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

float ewa_weight_gaussian_exp(float r2, float alpha) {
  float r2c = clamp(r2, 0.0f, 1.0f);
  return exp(-alpha * r2c);
}

float ewa_weight_gaussian_tent(float r2, float alpha) {
  if (r2 >= 1.0f) {
    return 0.0f;
  }
  float g = exp(-alpha * r2);
  float r = sqrt(r2);
  float fade = max(0.0f, 1.0f - r);  // linear to zero at r=1
  return g * fade;
}

float smootherstep(float x)
{
  float t = clamp(x, 0.0, 1.0);
  return ((6.0*t - 15.0)*t + 10.0) * t*t*t;
}

float ewa_weight_gaussian_smootherstep(float r2,
                                       float alpha,
                                       float r_soft)
{
  if (r2 >= 1.0f) {
    return 0.0f;
  }
  float g = exp(-alpha * r2);

  float r2_soft = r_soft * r_soft;
  if (r2 <= r2_soft) {
    return g;
  }

  float r = sqrt(r2);
  float t = (r - r_soft) / (1.0f - r_soft);
  float fade = 1.0f - smootherstep(t);
  return g * fade;
}

const uint EWA_EDGE_MODE_MASK        = 0x3u;
const uint EWA_EDGE_MODE_NONE        = 0x0u; // 00
const uint EWA_EDGE_MODE_TENT        = 0x1u; // 01
const uint EWA_EDGE_MODE_SMOOTHERSTEP= 0x2u; // 10

float compute_ewa_weight(float r2, uint fade_flags, float alpha, float r_soft) {
  uint mode = (fade_flags & EWA_EDGE_MODE_MASK);
  if (mode == EWA_EDGE_MODE_TENT) {
    return ewa_weight_gaussian_tent(r2, alpha);
  } else if (mode == EWA_EDGE_MODE_SMOOTHERSTEP) {
    return ewa_weight_gaussian_smootherstep(r2, alpha, r_soft);
  } else {
    return ewa_weight_gaussian_exp(r2, alpha);
  }
}

/* Normalized ellipse describing the footprint in texel space:
 * r^2(Δu,Δv) = A * Δu^2 + B * ΔuΔv + C * Δv^2   (inside if r^2 < 1)
 * s0..s1, t0..t1: integer bounding box to scan (inclusive)
 * texel_center_x, texel_center_y: center of the footprint in texel coordinates */
struct Ellipse {
  float A, B, C;
  float center_u_texel, center_v_texel;
  int s0, s1, t0, t1;
  bool valid;
};

/* Build the normalized ellipse and its integer bounding box from:
 *   - image_dimensions:        (width, height) in texels
 *   - coordinates:             uv center in [0,1]^2
 *   - du, dv:                  partials d(uv)/dx and d(uv)/dy (normalized)
 *   - max_ratio_between_axes: clamp for very skinny footprints
 *
 * We follow the standard PBRT formulation:
 *   A = (dv/dx)^2 + (dv/dy)^2 + 1
 *   B = -2 * [(du/dx)(dv/dx) + (du/dy)(dv/dy)]
 *   C = (du/dx)^2 + (du/dy)^2 + 1
 * then normalize so "inside" is r^2 < 1. */
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

  /* We add one to the A/C coefficients, because we sum over a discrete grid (one sample per texel center).
  A texel can still contribute even if its center lies just outside the true ellipse,
  as long as its 1×1 area overlaps the ellipse. If we only test the true ellipse we will miss those border
  texels, causing artefacts. */
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

/* Implementation based on PBRT:
 * https://github.com/mmp/pbrt-v4/blob/f140d7cba5dc7b941f9346d6b7d1476a05c28c37/src/pbrt/util/mipmap.cpp#L301
 * Book excerpt:
 * https://pbr-book.org/4ed/Textures_and_Materials/Image_Texture#fragment-ComputeEWAellipseaxes-0
 */
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
        float weight = compute_ewa_weight(r2, EWA_EDGE_MODE_NONE, 2.0f, 0.92f);
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
