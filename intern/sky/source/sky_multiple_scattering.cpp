/* SPDX-FileCopyrightText: 2011-2020 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/** \file
 * \ingroup intern_sky_modal
 */

#include "sky_math.h"
#include "sky_model.h"

/* Earth's atmosphere parameters. */
/* Ground reflectance */
static const float4 ground_albedo = make_float4(0.3f, 0.3f, 0.3f, 0.3f);
static const float INV_4PI = M_1_PI_F / 4.0f;
static const float phase_isotropic = INV_4PI;
static const float rayleigh_phase_scale = (3.0f / 16.0f) * M_1_PI_F;
/* Aerosols anisotropy */
static const float g = 0.8f;
static const float sqr_g = g * g;
/* Earth radius (km) */
static const float earth_radius = 6371.0f;
/* Atmosphere thickness (km) */
static const float atmosphere_thickness = 100.0f;
static const float atmosphere_radius = earth_radius + atmosphere_thickness;
/* Ray marching steps. Higher steps means increased accuracy but worse performance. */
static const int transmittance_steps = 32;
static const int in_scattering_steps = 32;

/* LUTs */
static const int transmittance_res_x = 256;
static const int transmittance_res_y = 64;
float transmittance_lut[transmittance_res_x][transmittance_res_y][4] = {};
static const float2 transmittance_res = make_float2((float)transmittance_res_x,
                                                    (float)transmittance_res_y);
static const float2 sky_res = make_float2(512.0f, 256.0f);  // same as the precomputed sky

/* Spectral data sampled at 630, 560, 490, 430 nm. */
static const float4 sun_spectral_irradiance = make_float4(1.679f, 1.828f, 1.986f, 1.307f);
static const float4 molecular_scattering_coefficient_base = make_float4(
    6.605e-3f, 1.067e-2f, 1.842e-2f, 3.156e-2f);
static const float4 ozone_absorption_cross_section = make_float4(
    3.472e-25f, 3.914e-25f, 1.349e-25f, 11.03e-27f);
static const float ozone_mean_monthly_dobson = 347.0f;
static const float4 aerosol_absorption_cross_section = make_float4(
    2.8722e-24f, 4.6168e-24f, 7.9706e-24f, 1.3578e-23f);
static const float4 aerosol_scattering_cross_section = make_float4(
    1.5908e-22f, 1.7711e-22f, 2.0942e-22f, 2.4033e-22f);
static const float aerosol_base_density = 1.3681e20f;
static const float aerosol_background_density = 2e6f;
static const float aerosol_height_scale = 0.73f;
static const float spectral_xyz[][4] = {
    {53.386917738564668023, 43.904844466369358263, 1.6137278251608962005, 20.762668673810577145},
    {22.981337506691024754, 71.347795700053393866, 18.422960591455485011, 2.3614213523314368527},
    {-0.0000003663162907346, 0.102506867965741307, 31.742921188390805758, 110.48009643252140334}};

static float clamp(float x, float min, float max)
{
  if (x < min) {
    return min;
  }
  else if (x > max) {
    return max;
  }
  else {
    return x;
  }
}

static float sign(float x)
{
  if (x < 0.0f) {
    return -1.0f;
  }
  else if (x == 0.0f) {
    return 0.0f;
  }
  else {
    return 1.0f;
  }
}

static float mix(float x, float y, float a)
{
  return x * (1.0f - a) + y * a;
}

static float4 interpolation(float2 uv)
{
  /* bilinear interpolation */
  float posx = float(transmittance_res_x - 1) * uv.x;
  float posy = float(transmittance_res_y - 1) * (1.0f - uv.y);
  int x1 = floorf(posx);
  int y1 = floorf(posy);
  int x2 = ceilf(posx);
  int y2 = ceilf(posy);
  float weight_x = posx - x1;
  float weight_y = posy - y1;
  float4 avg1 = make_float4(
      mix(transmittance_lut[x1][y1][0], transmittance_lut[x2][y1][0], weight_x),
      mix(transmittance_lut[x1][y1][1], transmittance_lut[x2][y1][1], weight_x),
      mix(transmittance_lut[x1][y1][2], transmittance_lut[x2][y1][2], weight_x),
      mix(transmittance_lut[x1][y1][3], transmittance_lut[x2][y1][3], weight_x));
  float4 avg2 = make_float4(
      mix(transmittance_lut[x1][y2][0], transmittance_lut[x2][y2][0], weight_x),
      mix(transmittance_lut[x1][y2][1], transmittance_lut[x2][y2][1], weight_x),
      mix(transmittance_lut[x1][y2][2], transmittance_lut[x2][y2][2], weight_x),
      mix(transmittance_lut[x1][y2][3], transmittance_lut[x2][y2][3], weight_x));

  return avg1 * (1.0f - weight_y) + avg2 * weight_y;
}

static float4 transmittance_from_lut(float cos_theta, float normalized_altitude)
{
  float u = clamp(cos_theta * 0.5f + 0.5f, 0.0f, 1.0f);
  float v = clamp(normalized_altitude, 0.0f, 1.0f);
  float2 uv = make_float2(u, v);
  return interpolation(uv);
}

static float ray_sphere_intersection(float3 ro, float3 rd, float radius)
{
  float b = dot(ro, rd);
  float c = dot(ro, ro) - radius * radius;
  if (c > 0.0f && b > 0.0f) {
    return -1.0f;
  }
  float d = b * b - c;
  if (d < 0) {
    return -1.0f;
  }
  if (d > b * b) {
    return -b + sqrtf(d);
  }
  return -b - sqrtf(d);
}

static float molecular_phase_function(float cos_theta)
{
  return rayleigh_phase_scale * (1.0f + cos_theta * cos_theta);
}

static float aerosol_phase_function(float cos_theta)
{
  float den = 1.0f + sqr_g + 2.0f * g * cos_theta;
  return INV_4PI * (1.0f - sqr_g) / (den * sqrtf(den));
}

static float4 get_multiple_scattering(float cos_theta, float normalized_height, float d)
{
  /* Solid angle subtended by the planet from a point at d distance from the planet center. */
  float omega = 2.0f * M_PI_F * (1.0f - sqrtf(d * d - earth_radius * earth_radius) / d);
  float4 T_to_ground = transmittance_from_lut(cos_theta, 0.0f);
  float4 T_ground_to_sample = transmittance_from_lut(1.0f, 0.0f) /
                              transmittance_from_lut(1.0f, normalized_height);
  /* 2nd order scattering from the ground. */
  float4 L_ground = phase_isotropic * omega * (ground_albedo / M_PI_F) * T_to_ground *
                    T_ground_to_sample * cos_theta;
  /* Fit of Earth's multiple scattering coming from other points in the atmosphere. */
  float4 L_ms = 0.02f * make_float4(0.217f, 0.347f, 0.594f, 1.0f) *
                (1.0f / (1.0f + 5.0f * expf(-17.92f * cos_theta)));
  return L_ms + L_ground;
}

static float4 get_molecular_scattering_coefficient(float h)
{
  return molecular_scattering_coefficient_base * expf(-0.07771971f * powf(h, 1.16364243f));
}

static float4 get_molecular_absorption_coefficient(float h)
{
  h += 1e-4;  // avoid division by 0
  float t = logf(h) - 3.22261f;
  float density = 3.78547397e20f * (1.0f / h) * expf(-t * t * 5.55555555f);
  return ozone_absorption_cross_section * ozone_mean_monthly_dobson * density;
}

static float get_aerosol_density(float h)
{
  float division = aerosol_background_density / aerosol_base_density;
  return aerosol_base_density * (expf(-h / aerosol_height_scale) + division);
}

static void get_atmosphere_collision_coefficients(float h,
                                                  float4 &aerosol_absorption,
                                                  float4 &aerosol_scattering,
                                                  float4 &molecular_absorption,
                                                  float4 &molecular_scattering)
{
  float altitude = fmax(h, 0.0f);  // in case height is negative
  float aerosol_density = get_aerosol_density(altitude);
  aerosol_absorption = aerosol_absorption_cross_section * aerosol_density;
  aerosol_scattering = aerosol_scattering_cross_section * aerosol_density;
  molecular_absorption = get_molecular_absorption_coefficient(altitude);
  molecular_scattering = get_molecular_scattering_coefficient(altitude);
}

static float3 spectral_to_xyz(float4 L)
{
  float3 xyz = make_float3(0.0f, 0.0f, 0.0f);
  for (int i = 0; i < 4; i++) {
    xyz.x += spectral_xyz[0][i] * L[i];
    xyz.y += spectral_xyz[1][i] * L[i];
    xyz.z += spectral_xyz[2][i] * L[i];
  }
  return xyz;
}

static float4 transmittance_lut_calc(float2 coordinates)
{
  float2 uv = coordinates / transmittance_res;
  float sun_cos_theta = uv.x * 2.0f - 1.0f;
  float3 sun_dir = make_float3(-sqrtf(1.0f - sun_cos_theta * sun_cos_theta), 0.0f, sun_cos_theta);
  float distance_to_earth_center = mix(earth_radius, atmosphere_radius, uv.y);
  float3 ray_origin = make_float3(0.0f, 0.0f, distance_to_earth_center);
  float t_d = ray_sphere_intersection(ray_origin, sun_dir, atmosphere_radius);
  float dt = t_d / transmittance_steps;
  float4 result = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
  for (int i = 0; i < transmittance_steps; i++) {
    float t = (i + 0.5f) * dt;
    float3 x_t = ray_origin + sun_dir * t;
    float altitude = (x_t).length() - earth_radius;
    float4 aerosol_absorption, aerosol_scattering, molecular_absorption, molecular_scattering;
    get_atmosphere_collision_coefficients(altitude,
                                          aerosol_absorption,
                                          aerosol_scattering,
                                          molecular_absorption,
                                          molecular_scattering);
    float4 extinction = aerosol_absorption + aerosol_scattering + molecular_absorption +
                        molecular_scattering;
    result = result + extinction * dt;
  }

  float4 transmittance = make_float4(
      expf(-result.x), expf(-result.y), expf(-result.z), expf(-result.w));

  return transmittance;
}

static float4 compute_inscattering(float3 sun_dir, float3 ray_origin, float3 ray_dir, float t_d)
{
  float cos_theta = dot(-ray_dir, sun_dir);
  float molecular_phase = molecular_phase_function(cos_theta);
  float aerosol_phase = aerosol_phase_function(cos_theta);
  float dt = t_d / in_scattering_steps;
  float4 L_inscattering = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
  float4 transmittance = make_float4(1.0f, 1.0f, 1.0f, 1.0f);
  for (int i = 0; i < in_scattering_steps; i++) {
    float t = (i + 0.5f) * dt;
    float3 x_t = ray_origin + ray_dir * t;
    float distance_to_earth_center = (x_t).length();
    float3 zenith_dir = x_t / distance_to_earth_center;
    float altitude = distance_to_earth_center - earth_radius;
    float normalized_altitude = altitude / atmosphere_thickness;
    float sample_cos_theta = dot(zenith_dir, sun_dir);
    float4 aerosol_absorption, aerosol_scattering, molecular_absorption, molecular_scattering;
    get_atmosphere_collision_coefficients(altitude,
                                          aerosol_absorption,
                                          aerosol_scattering,
                                          molecular_absorption,
                                          molecular_scattering);
    float4 extinction = aerosol_absorption + aerosol_scattering + molecular_absorption +
                        molecular_scattering;
    float4 transmittance_to_sun = transmittance_from_lut(sample_cos_theta, normalized_altitude);
    float4 ms = get_multiple_scattering(
        sample_cos_theta, normalized_altitude, distance_to_earth_center);
    float4 S = sun_spectral_irradiance *
               (molecular_scattering * (molecular_phase * transmittance_to_sun + ms) +
                aerosol_scattering * (aerosol_phase * transmittance_to_sun + ms));
    float4 step_transmittance = make_float4(expf(-dt * extinction.x),
                                            expf(-dt * extinction.y),
                                            expf(-dt * extinction.z),
                                            expf(-dt * extinction.w));
    /* Energy-conserving analytical integration "Physically Based Sky, Atmosphere and Cloud
     * Rendering in Frostbite" by Sébastien Hillaire. */
    float4 cut_ext = make_float4(fmax(extinction.x, 1e-7),
                                 fmax(extinction.y, 1e-7),
                                 fmax(extinction.z, 1e-7),
                                 fmax(extinction.w, 1e-7));
    float4 S_int = (S - S * step_transmittance) / cut_ext;
    L_inscattering = L_inscattering + transmittance * S_int;
    transmittance = transmittance * step_transmittance;
  }

  return L_inscattering;
}

static float3 sky_lut(float3 sun_dir, float2 coordinates, float altitude)
{
  float2 uv = coordinates / sky_res;
  float azimuth = 2.0f * M_PI_F * uv.x;
  /* Apply a non-linear transformation to the elevation to dedicate more texels to the horizon,
   * where having more detail matters. */
  float l = uv.y * 2.0f - 1.0f;
  float elev = l * l * sign(l) * M_PI_F * 0.5f;  // [-pi/2, pi/2]
  float3 ray_dir = make_float3(cosf(elev) * cosf(azimuth), cosf(elev) * sinf(azimuth), sinf(elev));
  float3 ray_origin = make_float3(0.0f, 0.0f, earth_radius + altitude);
  float atmos_dist = ray_sphere_intersection(ray_origin, ray_dir, atmosphere_radius);
  float ground_dist = ray_sphere_intersection(ray_origin, ray_dir, earth_radius);
  /* If no ground collision then use the distance to the outer atmosphere, else we have a collision
   * with the ground so we use the distance to it. */
  float t_d = (ground_dist < 0.0f) ? atmos_dist : ground_dist;
  float4 L = compute_inscattering(sun_dir, ray_origin, ray_dir, t_d);
  float3 xyz = spectral_to_xyz(L);

  return xyz;
}

void SKY_multiple_scattering_precompute_texture(float *pixels,
                                                int stride,
                                                int start_y,
                                                int end_y,
                                                int width,
                                                int height,
                                                float sun_elevation,
                                                float altitude,
                                                float air_density,
                                                float dust_density,
                                                float ozone_density)
{
  int half_width = width / 2;
  float sun_zenith_cos_angle = cosf(M_PI_2_F - sun_elevation);
  float3 sun_dir = make_float3(
      -sqrtf(1.0f - sun_zenith_cos_angle * sun_zenith_cos_angle), 0.0f, sun_zenith_cos_angle);

  for (int y = start_y; y < end_y; y++) {
    float *pixel_row = pixels + (y * width * stride);
    for (int x = 0; x < half_width; x++) {
      float2 coordinates = make_float2(x + 0.5f, y + 0.5f);
      float3 sky = sky_lut(sun_dir, coordinates, altitude / 1000.0f);

      /* Store pixels */
      int pos_x = x * stride;
      pixel_row[pos_x] = sky.x;
      pixel_row[pos_x + 1] = sky.y;
      pixel_row[pos_x + 2] = sky.z;
      /* Mirror sky */
      int mirror_x = (width - x - 1) * stride;
      pixel_row[mirror_x] = sky.x;
      pixel_row[mirror_x + 1] = sky.y;
      pixel_row[mirror_x + 2] = sky.z;
    }
  }
}

void SKY_multiple_scattering_precompute_transmittance()
{
  /* Calculate and store transmittance LUT */
  for (int x = 0; x < transmittance_res_x; x++) {
    for (int y = 0; y < transmittance_res_y; y++) {
      float2 coordinates = make_float2(x + 0.5f, y + 0.5f);
      float4 lut = transmittance_lut_calc(coordinates);
      int reverse_y = transmittance_res_y - y - 1;
      transmittance_lut[x][reverse_y][0] = lut.x;
      transmittance_lut[x][reverse_y][1] = lut.y;
      transmittance_lut[x][reverse_y][2] = lut.z;
      transmittance_lut[x][reverse_y][3] = lut.w;
    }
  }
}
