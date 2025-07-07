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

  if (sky_type == 0.0) {
    /* Single Scattering */
    if (co.z < -0.4) {
      /* too far below the horizon, just return black */
      color = float4(0.0, 0.0, 0.0, 1.0);
    }
    else {
      /* evaluate longitudinal position on the map */
      constexpr float tau = 6.28318530717958647692;
      float x = (spherical.y + M_PI + sun_rotation) / tau;
      if (x > 1.0) {
        x -= 1.0;
      }

      float fade;
      float y;
      if (co.z < 0.0) {
        /* we're below the horizon, so extend the map by blending from values at the horizon
        * to zero according to a cubic falloff */
        fade = 1.0 + co.z * 2.5;
        fade = fade * fade * fade;
        y = 0.508;
      }
      else {
        /* we're above the horizon, so compute the lateral position by inverting the remapped
        * coordinates that are preserve to have more detail near the horizon. */
        fade = 1.0;
        y = sqrt(dir_elevation / M_PI_2) / 2.0 + 0.5;
      }

      /* look up color in the precomputed map and convert to RGB */
      xyz = fade * texture(ima, float3(x, y, layer)).rgb;
      color = float4(dot(xyz_to_r, xyz), dot(xyz_to_g, xyz), dot(xyz_to_b, xyz), 1);
    }
  }
  else {
    /* Multiple Scattering */
    /* evaluate longitudinal position on the map */
    constexpr float tau = 6.28318530717958647692;
    float x = (spherical.y + M_PI + sun_rotation) / tau;

    /* Undo the non-linear transformation from the sky LUT. */
    float dir_elevation_abs;
    if (dir_elevation < 0.0) {
      dir_elevation_abs = -dir_elevation;
    }
    else {
      dir_elevation_abs = dir_elevation;
    }
    float y = sqrt(dir_elevation_abs / M_PI_2) * signx(dir_elevation) * 0.5 + 0.5;
    
    /* look up color in the precomputed map and convert to RGB */
    xyz = texture(ima, float3(x, y, layer)).rgb;
    color = float4(dot(xyz_to_r, xyz), dot(xyz_to_g, xyz), dot(xyz_to_b, xyz), 1);
  }
}
