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
CCL_NAMESPACE_END
