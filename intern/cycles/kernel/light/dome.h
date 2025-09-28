/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/camera/projection.h"
#include "kernel/globals.h"
#include "kernel/image.h"
#include "kernel/light/background.h"
#include "kernel/svm/image.h"

#include "util/math_float3.h"

CCL_NAMESPACE_BEGIN

/* Apply Euler rotation to direction vector for dome light */
ccl_device_inline float3 dome_light_apply_rotation(const float3 direction, const float3 rotation)
{
  /* Apply Euler rotations in ZYX order (yaw, pitch, roll) */
  const float cos_x = cosf(rotation.x), sin_x = sinf(rotation.x);
  const float cos_y = cosf(rotation.y), sin_y = sinf(rotation.y);
  const float cos_z = cosf(rotation.z), sin_z = sinf(rotation.z);

  /* Rotation matrix for ZYX Euler angles */
  const float3 row0 = make_float3(cos_y * cos_z, cos_y * sin_z, -sin_y);
  const float3 row1 = make_float3(
      sin_x * sin_y * cos_z - cos_x * sin_z, sin_x * sin_y * sin_z + cos_x * cos_z, sin_x * cos_y);
  const float3 row2 = make_float3(
      cos_x * sin_y * cos_z + sin_x * sin_z, cos_x * sin_y * sin_z - sin_x * cos_z, cos_x * cos_y);

  /* Apply rotation matrix to direction */
  return make_float3(dot(row0, direction), dot(row1, direction), dot(row2, direction));
}

/* Dome Light HDR texture sampling */

ccl_device_inline float3 dome_light_hdr_eval(KernelGlobals kg,
                                             const ccl_global KernelLight *klight,
                                             const float3 D)
{
  /* Check if HDR texture is available */
  const int tex_id = klight->dome.dome_hdr_tex;
  if (tex_id == -1) {
    /* No HDR texture, return neutral multiplier */
    return one_float3();
  }

  /* Additional safety check - verify texture ID is reasonable */
  if (tex_id < 0 || tex_id > 10000) { /* Arbitrary high limit */
    return one_float3();
  }

  /* Normalize direction and apply dome rotation before converting to UV coordinates */
  const float3 co = safe_normalize(D);
  const float3 rotated_co = dome_light_apply_rotation(co, klight->dome.dome_rotation);
  float2 uv = direction_to_equirectangular(rotated_co);

  /* Apply UV flipping if enabled */
  if (klight->dome.dome_hdr_flip_u) {
    uv.x = 1.0f - uv.x; /* Flip horizontally */
  }
  if (klight->dome.dome_hdr_flip_v) {
    uv.y = 1.0f - uv.y; /* Flip vertically */
  }

  /* Clamp UV coordinates to valid range [0,1] to prevent texture access issues */
  const float2 uv_clamped = make_float2(clamp(uv.x, 0.0f, 1.0f), clamp(uv.y, 0.0f, 1.0f));

  /* Use kernel_tex_image_interp directly for efficiency */
  const float4 hdr_color = kernel_tex_image_interp(kg, tex_id, uv_clamped.x, uv_clamped.y);

  /* Apply gamma correction to HDR color */
  const float gamma = klight->dome.dome_hdr_gamma;
  float3 corrected_color = make_float3(hdr_color.x, hdr_color.y, hdr_color.z);

  if (gamma != 1.0f && gamma > 0.0f) {
    const float inv_gamma = 1.0f / gamma;
    corrected_color.x = powf(max(corrected_color.x, 0.0f), inv_gamma);
    corrected_color.y = powf(max(corrected_color.y, 0.0f), inv_gamma);
    corrected_color.z = powf(max(corrected_color.z, 0.0f), inv_gamma);
  }

  /* Apply HDR intensity multiplier and return RGB */
  const float intensity = klight->dome.dome_hdr_strength;
  const float3 result = corrected_color * intensity;

  return result;
}

/* Sample dome light direction with importance sampling based on HDR content */
ccl_device_inline float3 dome_light_sample(KernelGlobals kg,
                                           const ccl_global KernelLight *klight,
                                           const float2 rand,
                                           ccl_private float *pdf)
{
  /* Check if we have CDF data for importance sampling */
  if (klight->dome.map_res_x > 0 && klight->dome.map_res_y > 0) {
    /* Use importance sampling based on HDR texture content */
    const int res_x = klight->dome.map_res_x;
    const int res_y = klight->dome.map_res_y;
    const int cdf_width = res_x + 1;

    /* Sample row (V direction) using marginal CDF - use exact same method as background */
    int first = 0;
    int count = res_y;
    while (count > 0) {
      const int step = count >> 1;
      const int middle = first + step;
      if (kernel_data_fetch(light_dome_marginal_cdf, middle).y < rand.y) {
        first = middle + 1;
        count -= step + 1;
      }
      else {
        count = step;
      }
    }
    const int index_v = max(0, first - 1);

    /* Sample column (U direction) using conditional CDF - use exact same method as background */
    first = 0;
    count = res_x;
    while (count > 0) {
      const int step = count >> 1;
      const int middle = first + step;
      if (kernel_data_fetch(light_dome_conditional_cdf, index_v * cdf_width + middle).y < rand.x) {
        first = middle + 1;
        count -= step + 1;
      }
      else {
        count = step;
      }
    }
    const int index_u = max(0, first - 1);

    /* Convert discrete coordinates to continuous UV coordinates */
    const float2 cdf_u = kernel_data_fetch(light_dome_conditional_cdf, index_v * cdf_width + index_u);
    const float2 cdf_next_u = kernel_data_fetch(light_dome_conditional_cdf, index_v * cdf_width + index_u + 1);
    const float2 cdf_v = kernel_data_fetch(light_dome_marginal_cdf, index_v);
    const float2 cdf_next_v = kernel_data_fetch(light_dome_marginal_cdf, index_v + 1);

    /* importance-sampled U and V directions using inverse lerp - same order as background */
    const float du = inverse_lerp(cdf_u.y, cdf_next_u.y, rand.x);
    const float dv = inverse_lerp(cdf_v.y, cdf_next_v.y, rand.y);

    const float pu = (index_u + du) / res_x;
    const float pv = (index_v + dv) / res_y;

    /* Convert UV to spherical coordinates (similar to environment texture) */
    const float phi = M_2PI_F * pu;
    const float theta = M_PI_F * pv;

    /* Convert to Cartesian direction */
    const float sin_theta = sinf(theta);
    const float cos_theta = cosf(theta);
    float3 D = make_float3(sin_theta * cosf(phi), sin_theta * sinf(phi), cos_theta);

    /* Apply inverse rotation to sampled direction to account for rotated HDR space */
    float3 inv_rotation = make_float3(-klight->dome.dome_rotation.x,
                                      -klight->dome.dome_rotation.y,
                                      -klight->dome.dome_rotation.z);
    D = dome_light_apply_rotation(D, inv_rotation);

    /* Compute PDF from CDF values - use same approach as background shader */
    const float2 cdf_last_u = kernel_data_fetch(light_dome_conditional_cdf, index_v * cdf_width + res_x);
    const float2 cdf_last_v = kernel_data_fetch(light_dome_marginal_cdf, res_y);

    const float denom = (M_2PI_F * M_PI_F * sin_theta) * cdf_last_u.x * cdf_last_v.x;
    if (denom == 0.0f) {
      *pdf = 0.0f;
    }
    else {
      *pdf = (cdf_u.x * cdf_v.x) / denom;
    }

    /* Ensure PDF is positive */
    *pdf = max(*pdf, 1e-6f);

    return D;
  }
  else {
    /* Fall back to uniform sphere sampling for dome lights without HDR textures */
    const float cos_theta = 1.0f - 2.0f * rand.x; /* cos(theta) in [-1, 1] */
    const float sin_theta = sqrtf(max(0.0f, 1.0f - cos_theta * cos_theta));
    const float phi = M_2PI_F * rand.y;

    /* Convert to Cartesian coordinates */
    const float3 D = make_float3(sin_theta * cosf(phi), sin_theta * sinf(phi), cos_theta);

    /* PDF for uniform sphere sampling - constant over all directions */
    *pdf = M_1_4PI_F; /* 1 / (4 * pi) */

    return D;
  }
}

/* Compute PDF for dome light sampling direction (for MIS) */
ccl_device_inline float dome_light_pdf(KernelGlobals kg,
                                       const ccl_global KernelLight *klight,
                                       const float3 D)
{
  /* Check if we have CDF data for importance sampling */
  if (klight->dome.map_res_x > 0 && klight->dome.map_res_y > 0) {
    /* Use importance sampling PDF based on HDR texture content */
    const int res_x = klight->dome.map_res_x;
    const int res_y = klight->dome.map_res_y;
    const int cdf_width = res_x + 1;

    /* Convert direction to spherical coordinates - apply rotation for consistency with eval */
    const float3 co = safe_normalize(D);
    const float3 rotated_co = dome_light_apply_rotation(co, klight->dome.dome_rotation);
    float2 uv = direction_to_equirectangular(rotated_co);

    /* Apply UV flipping if enabled - same as in eval function */
    if (klight->dome.dome_hdr_flip_u) {
      uv.x = 1.0f - uv.x; /* Flip horizontally */
    }
    if (klight->dome.dome_hdr_flip_v) {
      uv.y = 1.0f - uv.y; /* Flip vertically */
    }

    /* Clamp UV coordinates and convert to discrete indices */
    const float pu = clamp(uv.x, 0.0f, 1.0f);
    const float pv = clamp(uv.y, 0.0f, 1.0f);

    const int u = min((int)(pu * res_x), res_x - 1);
    const int v = min((int)(pv * res_y), res_y - 1);

    /* Use same optimized approach as background shader */
    const float sin_theta = sinf(pv * M_PI_F);
    if (sin_theta == 0.0f) {
      return 0.0f;
    }

    /* Get PDF values from CDF arrays - same approach as background */
    const float2 cdf_last_u = kernel_data_fetch(light_dome_conditional_cdf, v * cdf_width + res_x);
    const float2 cdf_last_v = kernel_data_fetch(light_dome_marginal_cdf, res_y);

    const float denom = (M_2PI_F * M_PI_F * sin_theta) * cdf_last_u.x * cdf_last_v.x;
    if (denom == 0.0f) {
      return 0.0f;
    }

    const float2 cdf_u = kernel_data_fetch(light_dome_conditional_cdf, v * cdf_width + u);
    const float2 cdf_v = kernel_data_fetch(light_dome_marginal_cdf, v);

    return max((cdf_u.x * cdf_v.x) / denom, 1e-6f);
  }
  else {
    /* Uniform sphere sampling PDF */
    return M_1_4PI_F; /* 1 / (4 * pi) */
  }
}

CCL_NAMESPACE_END
