/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/image.h"

#include "kernel/svm/types.h"
#include "kernel/svm/util.h"

#include "kernel/util/colorspace.h"

#include "util/color.h"

CCL_NAMESPACE_BEGIN

/* Sky texture */

ccl_device float sky_angle_between(const float thetav,
                                   const float phiv,
                                   const float theta,
                                   const float phi)
{
  const float cospsi = sinf(thetav) * sinf(theta) * cosf(phi - phiv) + cosf(thetav) * cosf(theta);
  return safe_acosf(cospsi);
}

/*
 * "A Practical Analytic Model for Daylight"
 * A. J. Preetham, Peter Shirley, Brian Smits
 */
ccl_device float sky_perez_function(const ccl_private float *lam,
                                    const float theta,
                                    const float gamma)
{
  const float ctheta = cosf(theta);
  const float cgamma = cosf(gamma);

  return (1.0f + lam[0] * expf(lam[1] / ctheta)) *
         (1.0f + lam[2] * expf(lam[3] * gamma) + lam[4] * cgamma * cgamma);
}

ccl_device float3 sky_radiance_preetham(KernelGlobals kg,
                                        const float3 dir,
                                        const float sunphi,
                                        const float suntheta,
                                        const float radiance_x,
                                        const float radiance_y,
                                        const float radiance_z,
                                        ccl_private float *config_x,
                                        ccl_private float *config_y,
                                        ccl_private float *config_z)
{
  /* convert vector to spherical coordinates */
  const float2 spherical = direction_to_spherical(dir);
  float theta = spherical.x;
  const float phi = -spherical.y + M_PI_2_F;

  /* angle between sun direction and dir */
  const float gamma = sky_angle_between(theta, phi, suntheta, sunphi);

  /* clamp theta to horizon */
  theta = min(theta, M_PI_2_F - 0.001f);

  /* compute xyY color space values */
  const float x = radiance_y * sky_perez_function(config_y, theta, gamma);
  const float y = radiance_z * sky_perez_function(config_z, theta, gamma);
  const float Y = radiance_x * sky_perez_function(config_x, theta, gamma);

  /* convert to RGB */
  const float3 xyz = xyY_to_xyz(x, y, Y);
  return xyz_to_rgb_clamped(kg, xyz);
}

/*
 * "An Analytic Model for Full Spectral Sky-Dome Radiance"
 * Lukas Hosek, Alexander Wilkie
 */
ccl_device float sky_radiance_internal(const ccl_private float *configuration,
                                       const float theta,
                                       const float gamma)
{
  const float ctheta = cosf(theta);
  const float cgamma = cosf(gamma);

  const float expM = expf(configuration[4] * gamma);
  const float rayM = cgamma * cgamma;
  const float mieM = (1.0f + rayM) / powf((1.0f + configuration[8] * configuration[8] -
                                           2.0f * configuration[8] * cgamma),
                                          1.5f);
  const float zenith = sqrtf(ctheta);

  return (1.0f + configuration[0] * expf(configuration[1] / (ctheta + 0.01f))) *
         (configuration[2] + configuration[3] * expM + configuration[5] * rayM +
          configuration[6] * mieM + configuration[7] * zenith);
}

ccl_device float3 sky_radiance_hosek(KernelGlobals kg,
                                     const float3 dir,
                                     const float sunphi,
                                     const float suntheta,
                                     const float radiance_x,
                                     const float radiance_y,
                                     const float radiance_z,
                                     ccl_private float *config_x,
                                     ccl_private float *config_y,
                                     ccl_private float *config_z)
{
  /* convert vector to spherical coordinates */
  const float2 spherical = direction_to_spherical(dir);
  float theta = spherical.x;
  const float phi = -spherical.y + M_PI_2_F;

  /* angle between sun direction and dir */
  const float gamma = sky_angle_between(theta, phi, suntheta, sunphi);

  /* clamp theta to horizon */
  theta = min(theta, M_PI_2_F - 0.001f);

  /* compute xyz color space values */
  const float x = sky_radiance_internal(config_x, theta, gamma) * radiance_x;
  const float y = sky_radiance_internal(config_y, theta, gamma) * radiance_y;
  const float z = sky_radiance_internal(config_z, theta, gamma) * radiance_z;

  /* convert to RGB and adjust strength */
  return xyz_to_rgb_clamped(kg, make_float3(x, y, z)) * (M_2PI_F / 683);
}

/* Nishita improved sky model */
ccl_device float3 geographical_to_direction(const float lat, const float lon)
{
  return spherical_to_direction(lat - M_PI_2_F, lon - M_PI_2_F);
}

ccl_device float3 sky_radiance_nishita(KernelGlobals kg,
                                       const bool multiple_scattering,
                                       const float3 dir,
                                       const uint32_t path_flag,
                                       const ccl_private float *sky_data,
                                       const uint texture_id)
{
  float3 pixel_bottom = make_float3(sky_data[0], sky_data[1], sky_data[2]);
  float3 pixel_top = make_float3(sky_data[3], sky_data[4], sky_data[5]);
  const float sun_elevation = sky_data[6];
  const float sun_rotation = sky_data[7];
  const float angular_diameter = sky_data[8];
  const float sun_intensity = sky_data[9];
  const float earth_intersection_angle = sky_data[10];
  const bool sun_disc = (angular_diameter >= 0.0f);
  const float2 direction = direction_to_spherical(dir);
  const float3 sun_dir = spherical_to_direction(sun_elevation - M_PI_2_F, sun_rotation - M_PI_2_F);
  const float sun_dir_angle = precise_angle(dir, sun_dir);
  const float half_angular = angular_diameter * 0.5f;
  const float dir_elevation = M_PI_2_F - direction.x;
  float3 rgb_sun = make_float3(0.0f, 0.0f, 0.0f);

  /* If the ray is inside the Sun disc and is not occluded by Earth's surface, render it, otherwise
   * render the sky. Alternatively, ignore the Sun if we're evaluating the background texture. */
  if (sun_disc && sun_dir_angle < half_angular &&
      !((path_flag & PATH_RAY_IMPORTANCE_BAKE) && kernel_data.background.use_sun_guiding) &&
      dir_elevation > earth_intersection_angle)
  {
    const float y = ((dir_elevation - sun_elevation) / angular_diameter) + 0.5f;
    const float3 xyz = interp(pixel_bottom, pixel_top, y);
    /* Limb darkening, coefficient is 0.6 */
    const float limb_darkening = (1.0f -
                                  0.6f * (1.0f - sqrtf(1.0f - sqr(sun_dir_angle / half_angular))));
    rgb_sun = xyz_to_rgb_clamped(kg, xyz) * limb_darkening;
  }
  const float x = fractf((-direction.y - M_PI_2_F + sun_rotation) / M_2PI_F);
  float3 rgb_sky;
  if (!multiple_scattering && dir.z < 0.0f) {
    /* Fade ground to black for Single Scattering model and disable Sun disc below horizon */
    rgb_sun = make_float3(0.0f, 0.0f, 0.0f);
    if (dir.z < -0.4f) {
      rgb_sky = make_float3(0.0f, 0.0f, 0.0f);
    }
    else {
      float fade = powf(1.0f + dir.z * 2.5f, 3.0f);
      const float3 xyz = make_float3(kernel_tex_image_interp(kg, texture_id, x, 0.508f));
      rgb_sky = xyz_to_rgb_clamped(kg, xyz) * fade;
    }
  }
  else {
    /* Undo the non-linear transformation from the sky LUT */
    const float dir_elevation_abs = (dir_elevation < 0.0f) ? -dir_elevation : dir_elevation;
    const float y = sqrtf(dir_elevation_abs / M_PI_2_F) * copysignf(1.0f, dir_elevation) * 0.5f +
                    0.5f;
    const float3 xyz = make_float3(kernel_tex_image_interp(kg, texture_id, x, y));
    rgb_sky = xyz_to_rgb_clamped(kg, xyz);
  }

  return rgb_sun * sun_intensity + rgb_sky;
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
  const NodeSkyType sky_model = NodeSkyType(node.w);

  const float3 dir = stack_load_float3(stack, dir_offset);
  float3 rgb;

  /* Preetham and Hosek share the same data */
  if (sky_model == NODE_SKY_PREETHAM || sky_model == NODE_SKY_HOSEK) {
    /* Define variables */
    float sunphi;
    float suntheta;
    float radiance_x;
    float radiance_y;
    float radiance_z;
    float config_x[9];
    float config_y[9];
    float config_z[9];

    float4 data = read_node_float(kg, &offset);
    sunphi = data.x;
    suntheta = data.y;
    radiance_x = data.z;
    radiance_y = data.w;

    data = read_node_float(kg, &offset);
    radiance_z = data.x;
    config_x[0] = data.y;
    config_x[1] = data.z;
    config_x[2] = data.w;

    data = read_node_float(kg, &offset);
    config_x[3] = data.x;
    config_x[4] = data.y;
    config_x[5] = data.z;
    config_x[6] = data.w;

    data = read_node_float(kg, &offset);
    config_x[7] = data.x;
    config_x[8] = data.y;
    config_y[0] = data.z;
    config_y[1] = data.w;

    data = read_node_float(kg, &offset);
    config_y[2] = data.x;
    config_y[3] = data.y;
    config_y[4] = data.z;
    config_y[5] = data.w;

    data = read_node_float(kg, &offset);
    config_y[6] = data.x;
    config_y[7] = data.y;
    config_y[8] = data.z;
    config_z[0] = data.w;

    data = read_node_float(kg, &offset);
    config_z[1] = data.x;
    config_z[2] = data.y;
    config_z[3] = data.z;
    config_z[4] = data.w;

    data = read_node_float(kg, &offset);
    config_z[5] = data.x;
    config_z[6] = data.y;
    config_z[7] = data.z;
    config_z[8] = data.w;

    /* Compute Sky */
    if (sky_model == NODE_SKY_PREETHAM) {
      rgb = sky_radiance_preetham(kg,
                                  dir,
                                  sunphi,
                                  suntheta,
                                  radiance_x,
                                  radiance_y,
                                  radiance_z,
                                  config_x,
                                  config_y,
                                  config_z);
    }
    else {
      rgb = sky_radiance_hosek(kg,
                               dir,
                               sunphi,
                               suntheta,
                               radiance_x,
                               radiance_y,
                               radiance_z,
                               config_x,
                               config_y,
                               config_z);
    }
  }
  /* Nishita */
  else {
    float sky_data[11];
    const float3 dir = stack_load_float3(stack, dir_offset);
    float4 data = read_node_float(kg, &offset);
    sky_data[0] = data.x;
    sky_data[1] = data.y;
    sky_data[2] = data.z;
    sky_data[3] = data.w;
    data = read_node_float(kg, &offset);
    sky_data[4] = data.x;
    sky_data[5] = data.y;
    sky_data[6] = data.z;
    sky_data[7] = data.w;
    data = read_node_float(kg, &offset);
    sky_data[8] = data.x;
    sky_data[9] = data.y;
    sky_data[10] = data.z;
    const uint texture_id = __float_as_uint(data.w);

    /* Compute Sky */
    rgb = sky_radiance_nishita(
        kg, sky_model == NODE_SKY_MULTIPLE_SCATTERING, dir, path_flag, sky_data, texture_id);
  }

  stack_store_float3(stack, out_offset, rgb);
  return offset;
}

CCL_NAMESPACE_END
