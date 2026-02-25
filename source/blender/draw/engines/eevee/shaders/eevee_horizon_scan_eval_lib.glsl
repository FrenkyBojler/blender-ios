/* SPDX-FileCopyrightText: 2017-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Implementation of Horizon Based Global Illumination and Ambient Occlusion.
 *
 * This mostly follows the paper:
 * "Screen Space Indirect Lighting with Visibility Bitmask"
 * by Olivier Therrien, Yannick Levesque, Guillaume Gilet
 *
 * Expects `screen_radiance_tx` and `screen_normal_tx` to be bound if `HORIZON_OCCLUSION` is not
 * defined.
 */

#include "infos/eevee_tracing_infos.hh"

SHADER_LIBRARY_CREATE_INFO(eevee_horizon_scan)

#include "draw_shape_lib.glsl"
#include "draw_view_lib.glsl"
#include "eevee_horizon_scan_lib.bsl.hh"
#include "eevee_ray_types_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_spherical_harmonics_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

namespace eevee::horizon {

template<typename T> float3 sample_radiance(float2 uv)
{
  return float3(0.0f);
}
template<typename T> float3 sample_normal(float2 uv)
{
  return float3(0.0f);
}
template<typename T> T select_result(float occlusion, SphericalHarmonicL1 sh)
{
  return T(0.0f);
}

template float3 sample_radiance<float>(float2 uv);
template float3 sample_normal<float>(float2 uv);
template float select_result<float>(float occlusion, SphericalHarmonicL1 sh);

template<> float3 sample_radiance<SphericalHarmonicL1>(float2 uv)
{
  return texture(screen_radiance_tx, uv).rgb;
}
template<> float3 sample_normal<SphericalHarmonicL1>(float2 uv)
{
  return texture(screen_normal_tx, uv).rgb * 2.0f - 1.0f;
}
template<>
SphericalHarmonicL1 select_result<SphericalHarmonicL1>(float occlusion, SphericalHarmonicL1 sh)
{
  return sh;
}

/**
 * Scans the horizon in many directions and returns the indirect lighting radiance.
 * Returned lighting is stored inside the context in `_accum` members already normalized.
 * If `reversed` is set to true, the input normal must be negated.
 */
template<typename ResultT>
ResultT eval(float3 vP,
             float3 vN,
             float4 noise,
             float2 pixel_size,
             float search_distance,
             float thickness_near,
             float thickness_far,
             float angle_bias,
             const int slice_count,
             const int sample_count,
             const bool reversed,
             const bool ao_only)
{
  float3 vV = drw_view_incident_vector(vP);

  float2 v_dir;
  if (slice_count <= 2) {
    /* We cover half the circle because we trace in both directions. */
    v_dir = sample_circle(noise.x / float(2 * slice_count));
  }

  float weight_accum = 0.0f;
  float occlusion_accum = 0.0f;
  SphericalHarmonicL1 sh_accum = spherical_harmonics_L1_new();

#if defined(GPU_METAL)
/* NOTE: Full loop unroll hint increases performance on Apple Silicon. */
#  pragma clang loop unroll(full)
#endif
  for (int slice = 0; slice < slice_count; slice++) {
    if (slice_count > 2) {
      /* We cover half the circle because we trace in both directions. */
      v_dir = sample_circle(((float(slice) + noise.x) / float(2 * slice_count)));
    }

    /* Setup integration domain around V. */
    float3 vB = normalize(cross(vV, float3(v_dir, 0.0f)));
    float3 vT = cross(vB, vV);

    /* Bitmask representing the occluded sectors on the slice. */
    uint slice_bitmask = 0u;

    /* Angle between vN and the horizon slice plane. */
    float vN_angle;
    /* Length of vN projected onto the horizon slice plane. */
    float vN_length;

    projected_normal_to_plane_angle_and_length(vN, vV, vT, vB, vN_length, vN_angle);

    vN_angle += (noise.z - 0.5f) * (M_PI / 32.0f) * angle_bias;

    SphericalHarmonicL1 sh_slice = spherical_harmonics_L1_new();

    /* For both sides of the view vector. */
    for (int side = 0; side < 2; side++) {
      Ray ray;
      ray.origin = vP;
      ray.direction = float3((side == 0) ? v_dir : -v_dir, 0.0f);
      ray.max_time = search_distance;

      /* TODO(fclem): Could save some computation here by computing entry and exit point on the
       * screen at once and just scan through. */
      ScreenSpaceRay ssray = raytrace_screenspace_ray_create(ray, pixel_size);

#if defined(GPU_METAL) && defined(GPU_APPLE)
/* NOTE: Full loop unroll hint increases performance on Apple Silicon. */
#  pragma clang loop unroll(full)
#endif
      for (int j = 0; j < sample_count; j++) {
        /* Always cross at least one pixel. */
        float time = 1.0f + square((float(j) + noise.y) / float(sample_count)) * ssray.max_time;

        if (reversed) {
          /* We need to cross at least 2 pixels to avoid artifacts form the HiZ storing only the
           * max depth. The HiZ would need to contain the min depth instead to avoid this. */
          time += 1.0f;
        }

        float lod = 1.0f + saturate(float(j) - noise.w) * uniform_buf.ao.lod_factor;

        float2 sample_uv = ssray.origin.xy + ssray.direction.xy * time;
        float sample_depth = textureLod(hiz_tx, sample_uv * uniform_buf.hiz.uv_scale, lod).r;

        if (sample_depth == 1.0f && !reversed) {
          /* Skip background. Avoids making shadow on the geometry near the far plane. */
          continue;
        }

        /* Bias depth a bit to avoid self shadowing issues. */
        constexpr float bias = 2.0f * 2.4e-7f;
        sample_depth += reversed ? -bias : bias;

        float3 vP_sample_front = drw_point_screen_to_view(float3(sample_uv, sample_depth));
        float3 vP_sample_back = vP_sample_front - vV * thickness_near;

        float sample_distance;
        float3 vL_front = normalize_and_get_length(vP_sample_front - vP, sample_distance);
        float3 vL_back = normalize(vP_sample_back - vP);
        if (sample_distance > search_distance) {
          continue;
        }

        /* Ordered pair of angle. Minimum in X, Maximum in Y.
         * Front will always have the smallest angle here since it is the closest to the view. */
        float2 theta = acos_fast(float2(dot(vL_front, vV), dot(vL_back, vV)));
        theta.y = max(theta.x + thickness_far, theta.y);
        /* If we are tracing backward, the angles are negative. Swizzle to keep correct order. */
        theta = (side == 0) ? theta.xy : -theta.yx;

        float3 radiance = sample_radiance<ResultT>(sample_uv) * float(!ao_only);
        /* Take emitter surface normal into consideration. */
        float3 normal = sample_normal<ResultT>(sample_uv);
        /* Discard back-facing samples.
         * The 2 factor is to avoid loosing too much energy v(which is something not
         * explained in the paper...). Likely to be wrong, but we need a soft falloff. */
        float facing_weight = saturate(-dot(normal, vL_front) * 2.0f);

        /* Angular bias shrinks the visibility bitmask around the projected normal. */
        float2 biased_theta = (theta - vN_angle) * angle_bias;
        uint sample_bitmask = angles_to_bitmask(biased_theta);
        float weight_bitmask = bitmask_to_visibility_uniform(sample_bitmask & ~slice_bitmask);

        radiance *= facing_weight * weight_bitmask;
        spherical_harmonics_encode_signal_sample(
            vL_front, float4(radiance, weight_bitmask), sh_slice);

        slice_bitmask |= sample_bitmask;
      }
    }

    float occlusion_slice = bitmask_to_occlusion_cosine(slice_bitmask);
    /* Correct normal not on plane (Eq. 8 of GTAO paper). */
    occlusion_accum += occlusion_slice * vN_length;

    /* Use uniform visibility since this is what we use for near field lighting. */
    sh_accum = spherical_harmonics_madd(sh_slice, vN_length, sh_accum);

    weight_accum += vN_length;

    /* Rotate 90 degrees. */
    v_dir = orthogonal(v_dir);
  }

  float weight_rcp = safe_rcp(weight_accum);

  /* Weight by area of the sphere. This is expected for correct SH evaluation. */
  sh_accum = spherical_harmonics_mul(sh_accum, weight_rcp * 4.0f * M_PI);
  occlusion_accum *= weight_rcp;
  return select_result<ResultT>(occlusion_accum, sh_accum);
}

template float eval<float>(
    float3, float3, float4, float2, float, float, float, float, int, int, bool, bool);
template SphericalHarmonicL1 eval<SphericalHarmonicL1>(
    float3, float3, float4, float2, float, float, float, float, int, int, bool, bool);

}  // namespace eevee::horizon
