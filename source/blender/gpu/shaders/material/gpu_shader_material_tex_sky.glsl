/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float3 sky_spherical_coordinates(float3 dir)
{
  return float3(M_PI_2 - atan(dir.z, length(dir.xy)), atan(dir.x, dir.y), 0.0);
}

float signx(float x)
{
  if (x < 0.0) {
    return -1.0;
  }
  else if (x > 0.0) {
    return 1.0;
  }
  else {
    return 0.0;
  }
}

void node_tex_sky(float3 co,
                  float sky_type,
                  float sun_rotation,
                  float3 xyz_to_r,
                  float3 xyz_to_g,
                  float3 xyz_to_b,
                  sampler2DArray ima,
                  float layer,
                  out float4 color)
{
  float3 spherical = sky_spherical_coordinates(co);
  float3 xyz;
  float dir_elevation = M_PI_2 - spherical.x;

  if (sky_type == 0.0 && co.z < 0.0) {
    /* Black ground if Single Scattering model, otherwise get Sky LUT */
    if (co.z < -0.4) {
      color = float4(0.0, 0.0, 0.0, 1.0);
    }
    else {
      /* Ground fade */
      constexpr float tau = 6.28318530717958647692;
      float x = (spherical.y + M_PI + sun_rotation) / tau;
      if (x > 1.0) {
        x -= 1.0;
      }
      float fade = 1.0 + co.z * 2.5;
      fade = fade * fade * fade;
      float y = 0.508;
      
      /* look up color in the precomputed map and convert to RGB */
      xyz = fade * texture(ima, float3(x, y, layer)).rgb;
      color = float4(dot(xyz_to_r, xyz), dot(xyz_to_g, xyz), dot(xyz_to_b, xyz), 1);
    }
  }
  else {
    /* Sky */
    constexpr float tau = 6.28318530717958647692;
    float x = (spherical.y + M_PI + sun_rotation) / tau;
    /* Undo the non-linear transformation from the sky LUT. */
    float dir_elevation_sign;
    float dir_elevation_abs = (dir_elevation < 0.0) ? -dir_elevation : dir_elevation;
    if (dir_elevation < 0.0) {
      dir_elevation_sign = -1.0;
    }
    else if (dir_elevation > 0.0) {
      dir_elevation_sign = 1.0;
    }
    else {
      dir_elevation_sign = 0.0;
    }
    float y = sqrt(dir_elevation_abs / M_PI_2) * dir_elevation_sign / 2.0 + 0.5;

    /* look up color in the precomputed map and convert to RGB */
    xyz = texture(ima, float3(x, y, layer)).rgb;
    color = float4(dot(xyz_to_r, xyz), dot(xyz_to_g, xyz), dot(xyz_to_b, xyz), 1);
  }
}
