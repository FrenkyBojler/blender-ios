/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_math_geom_lib.glsl"
#include "eevee_ltc_lib.bsl.hh"
#include "eevee_ltc_lut_lib.bsl.hh"

/* Attenuation cutoff needs to be the same in the shadow loop and the light eval loop. */
#define LIGHT_ATTENUATION_THRESHOLD 1e-6f

float3 light_world_to_local_direction(LightData light, float3 L)
{
  return transform_direction_transposed(light.object_to_world, L);
}
float3 light_world_to_local_point(LightData light, float3 point)
{
  return transform_point_inversed(light.object_to_world, point);
}

struct LightVector {
  /* World space light vector. From the shading point to the light center. Normalized. */
  float3 L;
  /* Distance from the shading point to the light center. */
  float dist;
};

LightVector light_vector_get(LightData light, const bool is_directional, float3 P)
{
  LightVector lv;
  if (is_directional) {
    lv.L = light.sun().direction;
    lv.dist = 1.0f;
  }
  else {
    lv.L = light.position() - P;
    float inv_distance = inversesqrt(length_squared(lv.L));
    lv.L *= inv_distance;
    lv.dist = 1.0f / inv_distance;
  }
  return lv;
}

/* Light vector to the closest point in the light shape. */
LightVector light_shape_vector_get(LightData light, const bool is_directional, float3 P)
{
  if (!is_directional && is_area_light(light.type)) {
    LightAreaData area = light.area();

    float3 lP = transform_point_inversed(light.object_to_world, P);
    float2 ls_closest_point = lP.xy;
    if (light.type == LIGHT_ELLIPSE) {
      ls_closest_point /= max(1.0f, length(ls_closest_point / area.size));
    }
    else {
      ls_closest_point = clamp(ls_closest_point, -area.size, area.size);
    }
    float3 ws_closest_point = transform_point(light.object_to_world,
                                              float3(ls_closest_point, 0.0f));

    float3 L = ws_closest_point - P;
    float inv_distance = inversesqrt(length_squared(L));
    LightVector lv;
    lv.L = L * inv_distance;
    lv.dist = 1.0f / inv_distance;
    return lv;
  }
  /* TODO(@fclem): other light shape? */
  return light_vector_get(light, is_directional, P);
}

namespace eevee::light {

/**
 * Approximate the ratio of the area of intersection of two spherical caps divided by the area of
 * the smallest cap.
 */
float light_cone_cone_ratio(float cone_angle_1, float cone_angle_2, float angle_between_axes)
{
  /* From "Ambient Aperture Lighting" by Chris Oat
   * Slide 15.
   * Simplified since we divide by the solid angle of the smallest cone. */
  return smoothstep(
      cone_angle_1 + cone_angle_2, abs(cone_angle_1 - cone_angle_2), abs(angle_between_axes));
}

float linearstep(float edge0, float edge1, float x)
{
  return saturate((x - edge0) / (edge1 - edge0));
}

float ratio_cone_cone(float cone_angle_1, float cone_angle_2, float angle_between_axes)
{
  return light_cone_cone_ratio(cone_angle_1, cone_angle_2, angle_between_axes);
}

float area_cone(float cone_angle)
{
  return M_TAU * (1.0 - cos(cone_angle));
}

float area_lune(float lune_angle)
{
  return linearstep(0.0, M_TAU, abs(lune_angle));
}

float area_cone_cone(float cone_angle_1, float cone_angle_2, float angle_between_axes)
{
  return ratio_cone_cone(cone_angle_1, cone_angle_2, angle_between_axes) *
         area_cone(min(cone_angle_1, cone_angle_2));
}

float area_cone_hemisphere(float cone_angle, float angle_between_axes)
{
  float r = abs(cone_angle);
  float d = abs(angle_between_axes);
  d = clamp(d, M_PI_2 - r + 0.0001, M_PI_2 + r - 0.0001);
  return -2.0 * acos(cos(d) / sin(r)) - 2.0 * acos(-cos(d) * cos(r) / (sin(d) * sin(r))) * cos(r) +
         M_TAU;
}

float light_cone_lune_area(float spread_half_angle, float edge_angle_1, float edge_angle_2)
{
  float angle_hemi1 = edge_angle_1 + M_PI_2;
  float angle_hemi2 = edge_angle_2 - M_PI_2;
  float ratio_hemi1_cone_isect = area_cone_hemisphere(spread_half_angle, angle_hemi1) /
                                 min(area_cone_hemisphere(M_PI_2, angle_hemi1),
                                     area_cone(spread_half_angle));
  float ratio_hemi2_cone_isect = area_cone_hemisphere(spread_half_angle, angle_hemi2) /
                                 min(area_cone_hemisphere(M_PI_2, angle_hemi2),
                                     area_cone(spread_half_angle));
  return min(ratio_hemi1_cone_isect, ratio_hemi2_cone_isect);
}

float light_lune_cone_ratio(float spread_half_angle, float edge_angle_1, float edge_angle_2)
{
  return light_cone_lune_area(spread_half_angle, edge_angle_1, edge_angle_2);
}

/* https://www.shadertoy.com/view/4dVcR1 */

float2 msign(float2 x)
{
  return float2((x.x < 0.0) ? -1.0 : 1.0, (x.y < 0.0) ? -1.0 : 1.0);
}

float2 sdEllipseNearestPoint(float2 p, float2 ab)
{
  float2 p_sign = msign(p);
  // symmetry
  p = abs(p);

  // find root with Newton solver
  float2 q = ab * (p - ab);

  float w = (q.x < q.y) ? 1.570796327 : 0.0;
  for (int i = 0; i < 4; i++) {
    float2 cs = float2(cos(w), sin(w));
    float2 u = ab * float2(cs.x, cs.y);
    float2 v = ab * float2(-cs.y, cs.x);
    w = w + dot(p - u, v) / (dot(p - u, u) + dot(v, v));
  }

  return ab * float2(cos(w), sin(w)) * p_sign;
}

float2 sdEllipseFarthestPoint(float2 p, float2 ab)
{
  float2 p_sign = msign(p);
  // symmetry
  p = abs(p);

  // find root with Newton solver
  float2 q = ab * (p + ab);

  float w = 3.1415 + ((q.x < q.y) ? 1.570796327 : 0.0);
  for (int i = 0; i < 4; i++) {
    float2 cs = float2(cos(w), sin(w));
    float2 u = ab * float2(cs.x, cs.y);
    float2 v = ab * float2(-cs.y, cs.x);
    w = w + dot(p - u, v) / (dot(p - u, u) + dot(v, v));
  }

  return ab * float2(cos(w), sin(w)) * p_sign;
}

/* from Real-Time Area Lighting: a Journey from Research to Production
 * Stephen Hill and Eric Heitz */
float3 light_edge_integral_vec(float3 v1, float3 v2)
{
  float x = dot(v1, v2);
  float y = abs(x);

  float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
  float b = 3.4175940 + (4.1616724 + y) * y;
  float v = a / b;

  float theta_sintheta = (x > 0.0) ? v : 0.5 * inversesqrt(max(1.0 - x * x, 1e-7)) - v;

  return cross(v1, v2) * theta_sintheta;
}

float light_spread_angle_attenuation(LightData light, LightVector lv)
{
  LightAreaData area_light = light.area();

  float3 lL = light_world_to_local_direction(light, lv.L * lv.dist);
  float distance_to_plane = abs(lL.z);
  float2 area_size = area_light.size;

  if (light.type == LIGHT_RECT) {
    float3 corners[4];
    corners[0] = float3(area_size.x, -area_size.y, 0.0);
    corners[1] = float3(area_size.x, area_size.y, 0.0);
    corners[2] = -corners[0];
    corners[3] = -corners[1];

    corners[0] = normalize((corners[0] + lL) * float3(10.0f, 10.0f, 1.0f));
    corners[1] = normalize((corners[1] + lL) * float3(10.0f, 10.0f, 1.0f));
    corners[2] = normalize((corners[2] + lL) * float3(10.0f, 10.0f, 1.0f));
    corners[3] = normalize((corners[3] + lL) * float3(10.0f, 10.0f, 1.0f));

    float3 avg_dir;
    avg_dir = light_edge_integral_vec(corners[0], corners[1]);
    avg_dir += light_edge_integral_vec(corners[1], corners[2]);
    avg_dir += light_edge_integral_vec(corners[2], corners[3]);
    avg_dir += light_edge_integral_vec(corners[3], corners[0]);

    float form_factor = length(avg_dir);
    float avg_dir_z = (avg_dir / form_factor).z;

    float half_light_angle = acos(1.0 - form_factor);

    return light_cone_cone_ratio(area_light.spread_half_angle, half_light_angle, acos(avg_dir_z));
  }

  float r1 = distance(sdEllipseNearestPoint(lL.xy, area_size), lL.xy);
  float r2 = distance(sdEllipseFarthestPoint(lL.xy, area_size), lL.xy);

  bool inside_ellipse = length_squared(lL.xy / area_size) < 1.0;
  if (inside_ellipse) {
    /* Signed distance. */
    r1 = -r1;
  }

  /* TODO(fclem): Port fast_atanf from cycles. */
  float angle_1 = atan(r1, distance_to_plane);
  float angle_2 = atan(r2, distance_to_plane);

  float half_light_angle = abs(angle_1 - angle_2) / 2.0;
  float elevation_angle = (angle_1 + angle_2) / 2.0;

  return light_cone_cone_ratio(area_light.spread_half_angle, half_light_angle, elevation_angle);
}

}  // namespace eevee::light

/* ---------------------------------------------------------------------- */
/** \name Light Functions
 * \{ */

/* From Frostbite PBR Course
 * Distance based attenuation
 * http://www.frostbite.com/wp-content/uploads/2014/11/course_notes_moving_frostbite_to_pbr.pdf */
float light_influence_attenuation(float dist, float inv_sqr_influence)
{
  float factor = square(dist) * inv_sqr_influence;
  float fac = saturate(1.0f - square(factor));
  return square(fac);
}

float light_spot_attenuation(LightData light, float3 L)
{
  LightSpotData spot = light.spot();
  float3 lL = light_world_to_local_direction(light, L);
  float ellipse = inversesqrt(1.0f + length_squared(lL.xy * spot.spot_size_inv / lL.z));
  float spotmask = smoothstep(0.0f, 1.0f, ellipse * spot.spot_mul + spot.spot_bias);
  return (lL.z > 0.0f) ? spotmask : 0.0f;
}

float light_attenuation_common(LightData light, const bool is_directional, LightVector lv)
{
  if (is_directional) {
    return 1.0f;
  }
  if (is_spot_light(light.type)) {
    return light_spot_attenuation(light, lv.L);
  }
  if (is_area_light(light.type)) {
    return float(dot(lv.L, light.z_axis()) > 0.0f) *
           eevee::light::light_spread_angle_attenuation(light, lv);
  }
  return 1.0f;
}

float light_shape_radius(LightData light)
{
  if (is_sun_light(light.type)) {
    return light.sun().shape_radius;
  }
  if (is_area_light(light.type)) {
    return length(light.area().size);
  }
  return light.local().local.shape_radius;
}

/**
 * Fade light influence when surface is not facing the light.
 * This is needed because LTC leaks light at roughness not 0 or 1
 * when the light is below the horizon.
 * L is normalized vector to light shape center.
 * Ng is ideally the geometric normal.
 */
float light_attenuation_facing(
    LightData light, float3 L, float distance_to_light, float3 Ng, const bool is_transmission)
{
  /* Sine of angle between light center and light edge. */
  float sin_solid_angle = light_shape_radius(light) / distance_to_light;
  /* Sine of angle between light center and shading plane. */
  float sin_light_angle = dot(L, is_transmission ? -Ng : Ng);
  /* Do attenuation after the horizon line to avoid harsh cut
   * or biasing of surfaces without light bleeding. */
  float dist = sin_solid_angle + (is_transmission ? -sin_light_angle : sin_light_angle);
  return saturate((dist + 0.1f) * 10.0f);
}

float light_attenuation_surface(LightData light, const bool is_directional, LightVector lv)
{
  float result = light_attenuation_common(light, is_directional, lv);
  if (!is_directional) {
    result *= light_influence_attenuation(lv.dist,
                                          light.local().local.influence_radius_invsqr_surface);
  }
  return result;
}

float light_attenuation_volume(LightData light, const bool is_directional, LightVector lv)
{
  float result = light_attenuation_common(light, is_directional, lv);
  if (!is_directional) {
    result *= light_influence_attenuation(lv.dist,
                                          light.local().local.influence_radius_invsqr_volume);
  }
  return result;
}

/* Cheaper alternative than evaluating the LTC.
 * The result needs to be multiplied by BSDF or Phase Function. */
float light_point_light(LightData light, const bool is_directional, LightVector lv)
{
  if (is_directional) {
    return 1.0f;
  }
  /* Using "Point Light Attenuation Without Singularity" from Cem Yuksel
   * http://www.cemyuksel.com/research/pointlightattenuation/pointlightattenuation.pdf
   * http://www.cemyuksel.com/research/pointlightattenuation/
   */
  float d_sqr = square(lv.dist);
  float r_sqr = square(light.local().local.shape_radius);
  /* Using reformulation that has better numerical precision. */
  float power = 2.0f / (d_sqr + r_sqr + lv.dist * sqrt(d_sqr + r_sqr));

  if (is_area_light(light.type)) {
    /* Modulate by light plane orientation / solid angle. */
    power *= saturate(dot(light.z_axis(), lv.L));
  }
  return power;
}

/**
 * Return the radius of the disk at the sphere origin spanning the same solid angle as the sphere
 * from a given distance.
 * Assume `distance_to_sphere > sphere_radius`, otherwise return almost infinite radius.
 */
float light_sphere_disk_radius(float sphere_radius, float distance_to_sphere)
{
  /* The sine of the half-angle spanned by a sphere light is equal to the tangent of the
   * half-angle spanned by a disk light with the same radius. */
  return sphere_radius *
         inversesqrt(max(1e-8f, 1.0f - square(sphere_radius / distance_to_sphere)));
}

struct LightVertices {
  float3 v[4];
};

LightVertices light_shape_corners(LightData light, LightVector lv)
{
  float3 Px = light.x_axis();
  float3 Py = light.y_axis();

  LightVertices vertices;

  if (light.type == LIGHT_RECT) {
    LightAreaData area = light.area();

    vertices.v[0] = Px * area.size.x + Py * -area.size.y;
    vertices.v[1] = Px * area.size.x + Py * area.size.y;
    vertices.v[2] = -vertices.v[0];
    vertices.v[3] = -vertices.v[1];

    float3 L = mix(light.z_axis(), lv.L * lv.dist, area.spread_mix_fac);
    vertices.v[0] += L;
    vertices.v[1] += L;
    vertices.v[2] += L;
    vertices.v[3] += L;
  }
  else {
    if (!is_area_light(light.type)) {
      make_orthonormal_basis(lv.L, Px, Py);
    }

    float2 size;
    if (is_sphere_light(light.type)) {
      /* Spherical omni or spot light. */
      size = float2(light_sphere_disk_radius(light.local().local.shape_radius, lv.dist));
    }
    else if (is_oriented_disk_light(light.type)) {
      /* View direction-aligned disk. */
      size = float2(light.local().local.shape_radius);
    }
    else if (is_sun_light(light.type)) {
      size = float2(light.sun().shape_radius);
    }
    else {
      /* Area light. */
      size = float2(light.area().size);
    }

    vertices.v[0] = Px * -size.x + Py * -size.y;
    vertices.v[1] = Px * size.x + Py * -size.y;
    vertices.v[2] = -vertices.v[0];

    float3 L = lv.L * lv.dist;
    if (is_area_light(light.type)) {
      L = mix(light.z_axis(), L, light.area().spread_mix_fac);
    }
    vertices.v[0] += L;
    vertices.v[1] += L;
    vertices.v[2] += L;
  }
  return vertices;
}

float light_ltc(sampler2DArray utility_tx,
                LightData light,
                float3 N,
                float3 V,
                LightVector lv,
                float4 ltc_mat,
                LightVertices vertices)
{
  if (is_sphere_light(light.type) && lv.dist < light.local().local.shape_radius) {
    /* Inside the sphere light, integrate over the hemisphere. */
    return 1.0f;
  }

  float3x3 Minv = eevee::lut::ltc::unpack(ltc_mat);
  if (light.type == LIGHT_RECT) {
    return eevee::ltc::evaluate_quad(utility_tx, vertices.v, N, V, Minv);
  }
  return eevee::ltc::evaluate_disk(utility_tx, N, V, Minv, vertices.v);
}

/** \} */
