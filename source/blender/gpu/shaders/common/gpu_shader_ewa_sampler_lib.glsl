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
void clamp_anisotropy(inout float2 x_gradient,
                      inout float2 y_gradient,
                      float max_ratio_between_axes)
{
  /* Make du/dx the longer axis; clamp the shorter (du/dy) if needed. */
  float len_squared_dx = dot(x_gradient, x_gradient);
  float len_squared_dy = dot(y_gradient, y_gradient);
  if (len_squared_dx < len_squared_dy) {
    /* Swap axes. */
    float2 tmp = x_gradient;
    x_gradient = y_gradient;
    y_gradient = tmp;
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
    y_gradient *= scale;
  }
}

/* Ellipsoid describing the footprint in texel space:
 * r^2(dx,dy) = A * dx^2 + B * dxdy + C * dy^2   (inside if r^2 < 1)
 * lower_bound..upper_bound bounding box to scan (inclusive)
 * center: center of the footprint in texel coordinates */
struct Ellipse {
  float a, b, c;
  float2 center;
  int2 lower_bound;
  int2 upper_bound;
  bool valid;
};

/* Build the ellipsoid and its integer bounding box:
 * Follows the PBRT formulation:
 *   A = (dv/dx)^2 + (dv/dy)^2 + 1
 *   B = -2 * [(du/dx)(dv/dx) + (du/dy)(dv/dy)]
 *   C = (du/dx)^2 + (du/dy)^2 + 1
 * then normalize so "inside" is r^2 < 1. */
Ellipse build_ellipse(float2 coordinates,
                      float2 x_gradient,
                      float2 y_gradient,
                      float max_ratio_between_axes)
{
  /* Clamps the ellipsoid based on the ratio between the axes.
   * This leads to better performance, because thin ellipsoids will be adjusted,
   * but also leads to neglectable blurring. */
  if (max_ratio_between_axes > 1.0) {
    clamp_anisotropy(x_gradient, y_gradient, max_ratio_between_axes);
  }

  float dv_dx = x_gradient.y;
  float dv_dy = y_gradient.y;
  float du_dx = x_gradient.x;
  float du_dy = y_gradient.x;

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


  float conic_discriminant = -b * b + 4.0 * a * c;
  float inv_conic_discriminant = 1.0 / conic_discriminant;
  float2 extent = 2.0f * inv_conic_discriminant *
                     sqrt(float2(conic_discriminant * c, conic_discriminant * a));

  float2 center = coordinates - 0.5f;
  int2 lower_bound = int2(ceil(center - extent));
  int2 upper_bound = int2(floor(center + extent));

  Ellipse e;
  e.a = a;
  e.b = b;
  e.c = c;
  e.center = center;
  e.lower_bound = lower_bound;
  e.upper_bound = upper_bound;
  e.valid = all(lessThanEqual(lower_bound, upper_bound));
  return e;
}

/* Computes the elliptical weighted average for a given input texture with their
 * normalized uv coordinates and the normalized Jacobian (x_gradient, y_gradient). */
float4 texture_ewa(sampler2D input_tx, float2 uv_coordinates, float2 x_gradient, float2 y_gradient)
{
  /* TODO(Ben): After feedback, check in with users if they need these as accessible
   * paramemters. */
  constexpr float smoothness = 2.0f;
  constexpr float max_ratio_between_axes = 8.0f;

  /* Bring derivatives to texel space. */
  float2 size = float2(textureSize(input_tx, 0));

  /* Scale the gradients back into texel space. */
  float2 coordinates = uv_coordinates * size;
  x_gradient *= size;
  y_gradient *= size;

  /* Build ellipsoid. */
  Ellipse ellipse = build_ellipse(
      coordinates, x_gradient, y_gradient, max_ratio_between_axes);

  /* Check whether ellipse is degenerative or numerically unstable. */
  if (!ellipse.valid) {
    return float4(0.0f);
  }

  float4 accum_rgba = float4(0.0f);
  float accum_weight = 0.0f;

  for (int y = ellipse.lower_bound.y; y <= ellipse.upper_bound.y; ++y) {
    float dy = float(y) - ellipse.center.y;
    float c_dy = ellipse.c * square(dy);
    float b_dy = ellipse.b * dy;

    for (int x = ellipse.lower_bound.x; x <= ellipse.upper_bound.x; ++x) {
      float dx = float(x) - ellipse.center.x;
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
