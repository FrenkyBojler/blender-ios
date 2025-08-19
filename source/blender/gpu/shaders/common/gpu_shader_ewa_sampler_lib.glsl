/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
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
Ellipse build_ellipse(float2 uv_center, float2 du_dx_texels, float2 du_dy_texels, float max_ratio_between_axes)
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
  as long as its 1x1 area overlaps the ellipse. If we only test the true ellipse we will miss those border
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

  float center_u_texel = uv_center.x - 0.5;
  float center_v_texel = uv_center.y - 0.5;

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
float4 texture_ewa(sampler2D input_tx, float2 coordinates, float2 x_gradient, float2 y_gradient)
{
  float alpha = 2.0f;
  float max_ratio_between_axes = 8.0f;

  /* Bring derivatives to texel space */
  float2 size = float2(textureSize(input_tx, 0));

  float2 uv_center = coordinates * size;
  float2 du_dx_texels = x_gradient * size;
  float2 du_dy_texels = y_gradient * size;

  /* Build ellipse & bbox in texel space */
  Ellipse ellipse = build_ellipse(uv_center, du_dx_texels, du_dy_texels, max_ratio_between_axes);

  if (!ellipse.valid) {
    return float4(0.0f);
  }

  float4 accum_rgba = float4(0.0f);
  float accum_weight = 0.0f;

  /* Incremental evaluation across each scanline */
  for (int v = ellipse.t0; y <= ellipse.t1; ++y) {
    float delta_v = float(b) - ellipse.center_v_texel;
    float C_delta_v = ellipse.C * delta_v * delta_v;
    float B_delta_v = ellipse.B * delta_v;

    for (int u = ellipse.s0; x <= ellipse.s1; ++x) {
      float delta_u = float(u) - ellipse.center_u_texel;
      float r2 = fma(ellipse.A, delta_u * delta_u, fma(B_delta_v, delta_u, C_delta_v));

      if (r2 < 1.0f) {
        float weight = exp(-alpha * r2);
        if (weight > 0.0f) {
          float4 rgba = texelFetch(input_tx, int2(u, v), 0);
          accum_rgba += weight * rgba;
          accum_weight += weight;
        }
      }
    }
  }

  float4 out_color = safe_divide(accum_rgba, accum_weight);
  return out_color;
}
