/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_math_geom_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_math_matrix_transform_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"
#include "gpu_shader_ray_lib.glsl"

#if 0
/* Screen-space ray ([0..1] "uv" range) where direction is normalize to be as small as one
 * full-resolution pixel. The ray is also clipped to all frustum sides.
 * Z component is device normalized Z (aka. depth buffer value).
 * W component is device normalized Z + Thickness.
 */
struct ScreenSpaceRay {
  float4 origin;
  float4 direction;
  float max_time;

};
#endif

/* Screen-space ray ([0..1] "uv" range) where direction is normalize to be as small as one
 * full-resolution pixel. The ray is also clipped to all frustum sides.
 * Z component is device normalized Z (aka. depth buffer value).
 * W component is device normalized Z + Thickness.
 */
struct ScreenSpaceRay {
  float4 origin;
  float4 direction;
  float max_time;

  static ScreenSpaceRay from_start_end(float4 hs_start, float4 hs_end, float2 pixel_size)
  {
    /* Constant bias (due to depth buffer precision). Helps with self intersection. */
    /* Magic numbers for 24bits of precision.
     * From http://terathon.com/gdc07_lengyel.pdf (slide 26) */
    constexpr float bias = -2.4e-7f * 2.0f;
    hs_start.z += bias;
    hs_end.z += bias;

    hs_start.xyz /= hs_start.w;
    hs_end.xyz /= hs_end.w;

    ScreenSpaceRay ray;
    ray.direction = hs_end - hs_start;
    ray.origin = hs_start;
    /* If the line is degenerate, make it cover at least one pixel
     * to not have to handle zero-pixel extent as a special case later */
    if (length_squared(ray.direction.xy) < 0.00001f) {
      ray.direction.xy = float2(0.0f, 0.00001f);
    }
    float ray_len_sqr = length_squared(ray.direction.xyz);
    /* Make direction cover one pixel. */
    bool is_more_vertical = abs(ray.direction.x / pixel_size.x) <
                            abs(ray.direction.y / pixel_size.y);
    ray.direction /= (is_more_vertical) ? abs(ray.direction.y) : abs(ray.direction.x);
    ray.direction *= (is_more_vertical) ? pixel_size.y : pixel_size.x;
    /* Clip to segment's end. */
    ray.max_time = sqrt(ray_len_sqr * safe_rcp(length_squared(ray.direction.xyz)));
    /* Clipping to frustum sides. */
    float clip_dist = line_unit_box_intersect_dist_safe(ray.origin.xyz, ray.direction.xyz);
    ray.max_time = min(ray.max_time, clip_dist);
    /* Convert to texture coords [0..1] range. */
    ray.origin.xyz = ray.origin.xyz * 0.5f + 0.5f;
    ray.direction.xyz *= 0.5f;
    return ray;
  }

  static ScreenSpaceRay create(Ray ray, float2 pixel_size)
  {
    float4 start = drw_point_view_to_homogenous(ray.origin);
    float4 end = drw_point_view_to_homogenous(ray.origin + ray.direction * ray.max_time);

    return ScreenSpaceRay::from_start_end(start, end, pixel_size);
  }

  static ScreenSpaceRay create(Ray ray, float4x4 winmat, float2 pixel_size)
  {
    float4 start = winmat * float4(ray.origin, 1.0f);
    float4 end = winmat * float4(ray.origin + ray.direction * ray.max_time, 1.0f);

    return ScreenSpaceRay::from_start_end(start, end, pixel_size);
  }

  float3 screen_position_at(float t)
  {
    return origin.xyz + direction.xyz * t;
  }
};
