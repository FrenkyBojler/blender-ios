/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "gpu_glsl_cpp_stubs.hh"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"

/* Implementation based on PBRT:
 * https://github.com/mmp/pbrt-v4/blob/f140d7cba5dc7b941f9346d6b7d1476a05c28c37/src/pbrt/util/mipmap.cpp#L301
 * Book excerpt:
 * https://pbr-book.org/4ed/Textures_and_Materials/Image_Texture#fragment-ComputeEWAellipseaxes-0
 */

/* Clamp anisotropy of the footprint: ensure the short axis is not more
 * than the `max_ratio_between_axes` times shorter than the long axis. Inputs are the
 * two derivative vectors in texel space (du/dx, dv/dx) and (du/dy, dv/dy). */
void clamp_anisotropy(inout float2 x_gradient_texel_space,
                      inout float2 y_gradient_texel_space,
                      float max_ratio_between_axes)
{
  /* Make du/dx the longer axis; clamp the shorter (du/dy) if needed. */
  float len_squared_dx = dot(x_gradient_texel_space, x_gradient_texel_space);
  float len_squared_dy = dot(y_gradient_texel_space, y_gradient_texel_space);
  if (len_squared_dx < len_squared_dy) {
    /* Swap axes. */
    float2 tmp = x_gradient_texel_space;
    x_gradient_texel_space = y_gradient_texel_space;
    y_gradient_texel_space = tmp;
    float tmpf = len_squared_dx;
    len_squared_dx = len_squared_dy;
    len_squared_dy = tmpf;
  }

  float long_len = sqrt(len_squared_dx);
  float short_len = sqrt(len_squared_dy);

  if (max_ratio_between_axes > 1.0 && short_len > 0.0 &&
      long_len > max_ratio_between_axes * short_len)
  {
    float scale = long_len / (max_ratio_between_axes * short_len);
    y_gradient_texel_space *= scale;
  }
}

/* Normalized ellipse describing the footprint in texel space:
 * r^2(dx,dy) = A * dx^2 + B * dxdy + C * dy^2   (inside if r^2 < 1)
 * s_lower_bound..s_upper_bound, t_lower_bound..t_upper_bound: integer
 * bounding box to scan (inclusive)
 * texel_center_x, texel_center_y: center of the footprint in texel coordinates */
struct Ellipse {
  float a, b, c;
  float2 uv_coord_center;
  int2 st_lower_bound;
  int2 st_upper_bound;
  bool valid;
};

/* Build the normalized ellipse and its integer bounding box from:
 * Follows the standard PBRT formulation:
 *   A = (dv/dx)^2 + (dv/dy)^2 + 1
 *   B = -2 * [(du/dx)(dv/dx) + (du/dy)(dv/dy)]
 *   C = (du/dx)^2 + (du/dy)^2 + 1
 * then normalize so "inside" is r^2 < 1. */
Ellipse build_ellipse(float2 uv_texel_space,
                      float2 x_gradient_texel_space,
                      float2 y_gradient_texel_space,
                      float max_ratio_between_axes)
{
  /* Clamps the ellipsoid based on the ratio between the axes.
   * This leads to better performance, because thin ellipsoids will be adjusted,
   * but also leads to neglectable blurring. */
  if (max_ratio_between_axes > 1.0) {
    clamp_anisotropy(x_gradient_texel_space, y_gradient_texel_space, max_ratio_between_axes);
  }

  float dv_dx = x_gradient_texel_space.y;
  float dv_dy = y_gradient_texel_space.y;
  float du_dx = x_gradient_texel_space.x;
  float du_dy = y_gradient_texel_space.x;

  /* We add one to the A/C coefficients, because we sum over a discrete grid
   * (one sample per texel center). A texel can still contribute even if its
   * center lies just outside the true ellipse, as long as its 1x1 area
   * overlaps the ellipse. If we only test the true ellipse we will miss those border
   * texels, causing artefacts. */
  float a = square(dv_dx) + square(dv_dy) + 1.0;
  float b = -2.0 * (du_dx * dv_dx + du_dy * dv_dy);
  float c = square(du_dx) + square(du_dy) + 1.0;

  /* Computes det(M) = AC - B^2 * 0.25. We require that the determinant is
   * positive define so r^2 =1 is a bounded ellipse.
   * If the determinant is <= 0, the footprint of the ellipsoid is invalid
   * (singluar Jacobian/extreme anisotropy).
   * https://en.wikipedia.org/wiki/Matrix_representation_of_conic_sections#Classification */
  float determinant = a * c - 0.25 * b * b;
  if (determinant <= 0.0) {
    Ellipse e;
    e.valid = false;
    return e;
  }

  float inv = 1.0 / determinant;
  a *= inv;
  b *= inv;
  c *= inv;

  float2 uv_coord_center = uv_texel_space - 0.5f;

  float conic_discriminant = -b * b + 4.0 * a * c;
  float inv_conic_discriminant = 1.0 / conic_discriminant;
  float2 uv_extent = 2.0f * inv_conic_discriminant *
                     sqrt(float2(conic_discriminant * c, conic_discriminant * a));
  int2 st_lower_bound = int2(ceil(uv_coord_center - uv_extent));
  int2 st_upper_bound = int2(floor(uv_coord_center + uv_extent));

  Ellipse e;
  e.a = a;
  e.b = b;
  e.c = c;
  e.uv_coord_center = uv_coord_center;
  e.st_lower_bound = st_lower_bound;
  e.st_upper_bound = st_upper_bound;
  e.valid = all(lessThanEqual(st_lower_bound, st_upper_bound));
  return e;
}

/* Computes the elliptical weighted average for a given input texture with their
 * normalized uv coordinates and the normalized Jacobian (x_gradient, y_gradient). */
float4 texture_ewa(sampler2D input_tx, float2 coordinates, float2 x_gradient, float2 y_gradient)
{
  /* TODO(Ben): After feedback, check in with users if they need these as accessible
   * paramemters. */
  constexpr float smoothness = 2.0f;
  constexpr float max_ratio_between_axes = 8.0f;

  /* Bring derivatives to texel space. */
  float2 size = float2(textureSize(input_tx, 0));

  /* Scale the gradients back into texel space. */
  float2 uv_texel_space = coordinates * size;
  float2 x_gradient_texel_space = x_gradient * size;
  float2 y_gradient_texel_space = y_gradient * size;

  /* Build ellipsoid. */
  Ellipse ellipse = build_ellipse(
      uv_texel_space, x_gradient_texel_space, y_gradient_texel_space, max_ratio_between_axes);

  /* Check whether ellipse is degenerative or numerically unstable. */
  if (!ellipse.valid) {
    return float4(0.0f);
  }

  float4 accum_rgba = float4(0.0f);
  float accum_weight = 0.0f;

  for (int y = ellipse.st_lower_bound.y; y <= ellipse.st_upper_bound.y; ++y) {
    float dy = float(y) - ellipse.uv_coord_center.y;
    float c_dy = ellipse.c * square(dy);
    float b_dy = ellipse.b * dy;

    for (int x = ellipse.st_lower_bound.x; x <= ellipse.st_upper_bound.x; ++x) {
      float dx = float(x) - ellipse.uv_coord_center.x;
      /* Based on the matrix representation of conic sections:
       * https://en.wikipedia.org/wiki/Matrix_representation_of_conic_sections
       * Evaluates r^2 = A*dx^2 + B*dx*dy + C*dy^2 */
      float r2 = ellipse.a * square(dx) + b_dy * dx + c_dy;

      if (r2 < 1.0f) {
        float weight = exp(-smoothness * r2);
        if (weight > 0.0f) {
          float4 rgba = texelFetch(input_tx, int2(x, y), 0);
          accum_rgba += weight * rgba;
          accum_weight += weight;
        }
      }
    }
  }

  return safe_divide(accum_rgba, accum_weight);
}
