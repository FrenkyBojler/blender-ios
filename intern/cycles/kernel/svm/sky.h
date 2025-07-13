/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/image.h"

#include "kernel/svm/util.h"

#include "kernel/util/colorspace.h"

#include "util/color.h"

CCL_NAMESPACE_BEGIN

/* Sky texture */

ccl_device float sign(float x)
{
  if (x < 0.0f) {
    return -1.0f;
  }
  else if (x > 0.0f) {
    return 1.0f;
  }
  else {
    return 0.0f;
  }
}

ccl_device float3 sky_radiance(KernelGlobals kg,
                               const bool multiple_scattering,
                               const float3 dir,
                               const uint32_t path_flag,
                               const float3 pixel_bottom,
                               const float3 pixel_top,
                               const ccl_private float *sky_data,
                               const uint texture_id)
{
  const float sun_elevation = sky_data[0];
  const float sun_rotation = sky_data[1];
  const float angular_diameter = sky_data[2];
  const float sun_intensity = sky_data[3];
  const bool sun_disc = (angular_diameter >= 0.0f);
  const float2 direction = direction_to_spherical(dir);
  const float3 sun_dir = spherical_to_direction(sun_elevation - M_PI_2_F, sun_rotation - M_PI_2_F);
  const float sun_dir_angle = precise_angle(dir, sun_dir);
  const float half_angular = angular_diameter * 0.5f;
  const float dir_elevation = M_PI_2_F - direction.x;
  float3 xyz;
  float y;

  /* If the ray is inside the sun disc, render it, otherwise render the sky.
   * Alternatively, ignore the sun if we're evaluating the background texture. */
  if (sun_disc && sun_dir_angle < half_angular &&
      !((path_flag & PATH_RAY_IMPORTANCE_BAKE) && kernel_data.background.use_sun_guiding))
  {
    y = ((dir_elevation - sun_elevation) / angular_diameter) + 0.5f;
    /* Limb darkening, coefficient is 0.6 */
    xyz = interp(pixel_bottom, pixel_top, y) * sun_intensity;
    const float limb_darkening = (1.0f -
                                  0.6f * (1.0f - sqrtf(1.0f - sqr(sun_dir_angle / half_angular))));
    xyz *= limb_darkening;
  }
  else {
    const float x = fractf((-direction.y - M_PI_2_F + sun_rotation) / M_2PI_F);
    if (!multiple_scattering && dir.z < 0.0f) {
      /* If Single Scattering is used, fade ground to black */
      if (dir.z < -0.4f) {
        xyz = make_float3(0.0f, 0.0f, 0.0f);
      }
      else {
        float fade = 1.0f + dir.z * 2.5f;
        fade = fade * fade * fade;
        xyz = make_float3(kernel_tex_image_interp(kg, texture_id, x, 0.508f)) * fade;
      }
    }
    else {
      /* Undo the non-linear transformation from the sky LUT */
      const float dir_elevation_abs = (dir_elevation < 0.0f) ? -dir_elevation : dir_elevation;
      y = sqrtf(dir_elevation_abs / M_PI_2_F) * sign(dir_elevation) * 0.5f + 0.5f;
      xyz = make_float3(kernel_tex_image_interp(kg, texture_id, x, y));
    }
  }

  return xyz_to_rgb_clamped(kg, xyz);
}

ccl_device_noinline int svm_node_tex_sky(KernelGlobals kg,
                                         const uint32_t path_flag,
                                         ccl_private float *stack,
                                         const uint4 node,
                                         int offset)
{
  /* Load data */
  const uint dir_offset = node.y;
  const uint out_offset = node.z;
  const bool multiple_scattering = node.w;
  const float3 dir = stack_load_float3(stack, dir_offset);
  float4 data = read_node_float(kg, &offset);
  const float3 pixel_bottom = make_float3(data.x, data.y, data.z);
  float3 pixel_top;
  pixel_top.x = data.w;
  float sky_data[4];
  data = read_node_float(kg, &offset);
  pixel_top.y = data.x;
  pixel_top.z = data.y;
  sky_data[0] = data.z;
  sky_data[1] = data.w;
  data = read_node_float(kg, &offset);
  sky_data[2] = data.x;
  sky_data[3] = data.y;
  const uint texture_id = __float_as_uint(data.z);

  /* Compute Sky */
  float3 f = sky_radiance(
      kg, multiple_scattering, dir, path_flag, pixel_bottom, pixel_top, sky_data, texture_id);

  stack_store_float3(stack, out_offset, f);
  return offset;
}

CCL_NAMESPACE_END
