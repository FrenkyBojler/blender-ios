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
  float x = (spherical.y + M_PI + sun_rotation) / (2.0 * M_PI);
  float fade = 1.0;
  float y;

  if (sky_type == 0.0 && co.z < -0.4) {
    /* Black ground if Single Scattering model */
    color = float4(0.0, 0.0, 0.0, 1.0);
    return;
  }
  if (sky_type == 0.0 && co.z < 0.0) {
    /* Ground fade */
    fade = pow(1.0 + co.z * 2.5, 3.0);
    y = 0.508;
  }
  else {
    /* Undo the non-linear transformation from the sky LUT. */
    float dir_elevation_abs = (dir_elevation < 0.0) ? -dir_elevation : dir_elevation;
    y = sqrt(dir_elevation_abs / M_PI_2) * signx(dir_elevation) * 0.5 + 0.5;
  }
  /* Look up color in the precomputed map and convert to RGB. */
  xyz = fade * texture(ima, float3(x, y, layer)).rgb;
  color = float4(dot(xyz_to_r, xyz), dot(xyz_to_g, xyz), dot(xyz_to_b, xyz), 1);
}
