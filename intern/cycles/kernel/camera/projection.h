/* SPDX-FileCopyrightText: 2009-2010 Sony Pictures Imageworks Inc., et al.
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Adapted code from Open Shading Language. */

#pragma once

#include "util/math.h"
#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Spherical coordinates <-> Cartesian direction. */

ccl_device float2 direction_to_spherical(float3 dir)
{
  float theta = safe_acosf(dir.z);
  float phi = atan2f(dir.x, dir.y);

  return make_float2(theta, phi);
}

ccl_device float3 spherical_to_direction(float theta, float phi)
{
  float sin_theta = sinf(theta);
  return make_float3(sin_theta * sinf(phi), sin_theta * cosf(phi), cosf(theta));
}

/* Equirectangular coordinates <-> Cartesian direction */

ccl_device float2 direction_to_equirectangular_range(float3 dir, float4 range)
{
  if (is_zero(dir))
    return zero_float2();

  float u = (atan2f(dir.y, dir.x) - range.y) / range.x;
  float v = (acosf(dir.z / len(dir)) - range.w) / range.z;

  return make_float2(u, v);
}

ccl_device float3 equirectangular_range_to_direction(float u, float v, float4 range)
{
  float phi = range.x * u + range.y;
  float theta = range.z * v + range.w;
  float sin_theta = sinf(theta);
  return make_float3(sin_theta * cosf(phi), sin_theta * sinf(phi), cosf(theta));
}

ccl_device float2 direction_to_equirectangular(float3 dir)
{
  return direction_to_equirectangular_range(dir, make_float4(-M_2PI_F, M_PI_F, -M_PI_F, M_PI_F));
}

ccl_device float3 equirectangular_to_direction(float u, float v)
{
  return equirectangular_range_to_direction(u, v, make_float4(-M_2PI_F, M_PI_F, -M_PI_F, M_PI_F));
}

ccl_device float2 direction_to_central_cylindrical(float3 dir, float4 range)
{
  const float z = dir.z / len(make_float2(dir.x, dir.y));
  const float theta = atan2f(dir.y, dir.x);
  const float u = inverse_lerp(range.x, range.y, theta);
  const float v = inverse_lerp(range.z, range.w, z);
  return make_float2(u, v);
}

ccl_device float3 central_cylindrical_to_direction(float u, float v, float4 range)
{
  const float theta = mix(range.x, range.y, u);
  const float z = mix(range.z, range.w, v);
  return make_float3(cosf(theta), sinf(theta), z);
}

/* Fisheye <-> Cartesian direction */

ccl_device float2 direction_to_fisheye(float3 dir, float fov)
{
  const float r = atan2f(len(make_float2(dir.y, dir.z)), dir.x) / fov;
  const float2 uv = r * safe_normalize(make_float2(dir.y, dir.z));
  return make_float2(0.5f - uv.x, uv.y + 0.5f);
}

using T = float;

typedef struct {
  T data[2];
} Mat_2x1;

typedef struct {
  T data[2][2];
} Mat_2x2;

// FisheyeRadTanThinPrism: f,cx,cy,k0,k1,k2,k3,k5,k5,p1,p2,s1,s2,s3,s4
static constexpr bool useSingleFocalLength = true;
static constexpr bool useTangential = true;
static constexpr bool useThinPrism = true;
static constexpr bool useSkew = false;
static constexpr int kFocalXIdx = 0;
static constexpr int kFocalYIdx = 1 - useSingleFocalLength;
static constexpr int numK = 6;
static constexpr int kPrincipalPointColIdx = 2 - useSingleFocalLength;
static constexpr int kPrincipalPointRowIdx = 3 - useSingleFocalLength;
static constexpr int startK = kPrincipalPointRowIdx + 1;
static constexpr int kSkewIdx = 4 - useSingleFocalLength;
static constexpr int startP = startK + numK;
static constexpr int startS = startP + 2 * useTangential;
static constexpr int kMaxIterations = 50;
static constexpr T converge_threshold = T(1e-8);
static constexpr T eps = T(1e-8);
static constexpr int kNumParams = (4 - useSingleFocalLength) + numK + 2 * useTangential +
                                  4 * useThinPrism + 1 * useSkew;

ccl_device_inline float3 fisheye_radtanthinprism_to_direction(
    float u, float v, float width, float height, const float *params0)
{

  T params[15];
  for (int i = 0; i < 15; i++) {
    params[i] = T(params0[i]);
  }

  T f = params[0];
  /* Principal point Y needs to be mirrored because this
   * projection assumes a +Z forward, +Y down and +X right convention.
   */
  // T cy = T(height) - params[2]; // - T(1.0);

  // get uvDistorted:
  Mat_2x1 uvDistorted;
  uvDistorted.data[0] = T((u - 0.5f) * width) / f;
  uvDistorted.data[1] = T((v - 0.5f) * height) / f;

  // initial guess
  Mat_2x1 xr_yr;
  memcpy(xr_yr.data, uvDistorted.data, sizeof(xr_yr.data));

  // do Newton iterations to find xr_yr
  for (int j = 0; j < kMaxIterations; ++j) {
    // compute the estimated uvDistorted
    Mat_2x1 uvDistorted_est;
    memcpy(uvDistorted_est.data, xr_yr.data, sizeof(uvDistorted_est.data));
    T xr_yr_squaredNorm = xr_yr.data[0] * xr_yr.data[0] + xr_yr.data[1] * xr_yr.data[1];

    if (useTangential) {
      const T *param = params + startP;
      T temp = T(2.0) * (xr_yr.data[0] * param[0] + xr_yr.data[1] * param[1]);
      uvDistorted_est.data[0] += temp * xr_yr.data[0] + xr_yr_squaredNorm * param[0];
      uvDistorted_est.data[1] += temp * xr_yr.data[1] + xr_yr_squaredNorm * param[1];
    }

    if (useThinPrism) {
      const T *param = params + startS;
      T radialPowers2And4[2];
      radialPowers2And4[0] = xr_yr_squaredNorm;
      radialPowers2And4[1] = xr_yr_squaredNorm * xr_yr_squaredNorm;
      uvDistorted_est.data[0] += param[0] * radialPowers2And4[0] + param[1] * radialPowers2And4[1];
      uvDistorted_est.data[1] += param[2] * radialPowers2And4[0] + param[3] * radialPowers2And4[1];
    }

    // compute the derivative of uvDistorted wrt xr_yr
    Mat_2x2 duvDistorted_dxryr;
    if (useTangential) {
      T offdiag = T(2.0) * (xr_yr.data[0] * params[startP + 1] + xr_yr.data[1] * params[startP]);
      duvDistorted_dxryr.data[0][0] = T(1.0) + T(6.0) * xr_yr.data[0] * params[startP] +
                                      T(2.0) * xr_yr.data[1] * params[startP + 1];
      duvDistorted_dxryr.data[0][1] = offdiag;
      duvDistorted_dxryr.data[1][0] = offdiag;
      duvDistorted_dxryr.data[1][1] = T(1.0) + T(6.0) * xr_yr.data[1] * params[startP + 1] +
                                      T(2.0) * xr_yr.data[0] * params[startP];
    }
    else {
      duvDistorted_dxryr.data[0][0] = T(1.0);
      duvDistorted_dxryr.data[0][1] = T(0.0);
      duvDistorted_dxryr.data[1][0] = T(0.0);
      duvDistorted_dxryr.data[1][1] = T(1.0);
    }

    if (useThinPrism) {
      T temp1 = T(2.0) * (params[startS] + T(2.0) * params[startS + 1] * xr_yr_squaredNorm);
      duvDistorted_dxryr.data[0][0] += xr_yr.data[0] * temp1;
      duvDistorted_dxryr.data[0][1] += xr_yr.data[1] * temp1;

      T temp2 = T(2.0) * (params[startS + 2] + T(2.0) * params[startS + 3] * xr_yr_squaredNorm);
      duvDistorted_dxryr.data[1][0] += xr_yr.data[0] * temp2;
      duvDistorted_dxryr.data[1][1] += xr_yr.data[1] * temp2;
    }

    // compute correction:
    // note: the matrix duvDistorted_dxryr will be close to identity (for reasonable values
    // of tangential/thin prism distortions), so using an analytical inverse here is safe
    Mat_2x1 correction;
    T determinant = duvDistorted_dxryr.data[0][0] * duvDistorted_dxryr.data[1][1] -
                    duvDistorted_dxryr.data[0][1] * duvDistorted_dxryr.data[1][0];
    correction.data[0] =
        (T(1.0) / determinant) *
        (duvDistorted_dxryr.data[1][1] * (uvDistorted.data[0] - uvDistorted_est.data[0]) -
         duvDistorted_dxryr.data[0][1] * (uvDistorted.data[1] - uvDistorted_est.data[1]));
    correction.data[1] =
        (T(1.0) / determinant) *
        (duvDistorted_dxryr.data[0][0] * (uvDistorted.data[1] - uvDistorted_est.data[1]) -
         duvDistorted_dxryr.data[1][0] * (uvDistorted.data[0] - uvDistorted_est.data[0]));

    xr_yr.data[0] += correction.data[0];
    xr_yr.data[1] += correction.data[1];

    const T err = correction.data[0] * correction.data[0] +
                  correction.data[1] * correction.data[1];
    if (err < converge_threshold) {
      break;
    }
  }

  // early exit if point is in the center of the image
  T xr_yrNorm = sqrt(xr_yr.data[0] * xr_yr.data[0] + xr_yr.data[1] * xr_yr.data[1]);
  if (xr_yrNorm == T(0.0)) {
    float3 point3dEst = make_float3(0.0f, 0.0f, 1.0f);
    return point3dEst;
  }

  // otherwise, find theta
  T th_radialDesired = xr_yrNorm;
  T theta = th_radialDesired;

  for (int j = 0; j < kMaxIterations; ++j) {
    T thetaSq = theta * theta;

    T th_radial = 1.0;
    T dthD_dth = 1.0;

    T theta2is = thetaSq;
    for (int i = 0; i < numK; ++i) {
      th_radial += theta2is * params[startK + i];
      dthD_dth += (2 * i + 3) * params[startK + i] * theta2is;
      theta2is *= thetaSq;
    }
    th_radial *= theta;

    T step;
    if (std::fabs(dthD_dth) > T(eps)) {
      step = (th_radialDesired - th_radial) / dthD_dth;
    }
    else {
      step = (th_radialDesired - th_radial) * dthD_dth > T(0.0) ? 10 * eps : -10 * eps;
    }

    theta += step;

    if (std::fabs(step) < T(eps)) {
      break;
    }

    if (std::fabs(theta) >= T(M_PI / 2.0)) {
      theta = T(0.999 * M_PI / 2.0);
    }
  }

  // get the point coordinates:
  float3 point3dEst = make_float3((float)(tan(theta) / xr_yrNorm * xr_yr.data[0]),
                                  (float)(tan(theta) / xr_yrNorm * xr_yr.data[1]),
                                  1.0f);

  return point3dEst;
}

ccl_device float3 fisheye_to_direction(float u, float v, float fov)
{
  u = (u - 0.5f) * 2.0f;
  v = (v - 0.5f) * 2.0f;

  float r = sqrtf(u * u + v * v);

  if (r > 1.0f)
    return zero_float3();

  float phi = safe_acosf((r != 0.0f) ? u / r : 0.0f);
  float theta = r * fov * 0.5f;

  if (v < 0.0f)
    phi = -phi;

  return make_float3(cosf(theta), -cosf(phi) * sinf(theta), sinf(phi) * sinf(theta));
}

ccl_device float2 direction_to_fisheye_equisolid(float3 dir, float lens, float width, float height)
{
  const float theta = safe_acosf(dir.x);
  const float r = 2.0f * lens * sinf(theta * 0.5f);

  const float2 uv = r * safe_normalize(make_float2(dir.y, dir.z));
  return make_float2(0.5f - uv.x / width, uv.y / height + 0.5f);
}

ccl_device_inline float3
fisheye_equisolid_to_direction(float u, float v, float lens, float fov, float width, float height)
{
  u = (u - 0.5f) * width;
  v = (v - 0.5f) * height;

  float rmax = 2.0f * lens * sinf(fov * 0.25f);
  float r = sqrtf(u * u + v * v);

  if (r > rmax)
    return zero_float3();

  float phi = safe_acosf((r != 0.0f) ? u / r : 0.0f);
  float theta = 2.0f * asinf(r / (2.0f * lens));

  if (v < 0.0f)
    phi = -phi;

  return make_float3(cosf(theta), -cosf(phi) * sinf(theta), sinf(phi) * sinf(theta));
}

ccl_device_inline float3 fisheye_lens_polynomial_to_direction(
    float u, float v, float coeff0, float4 coeffs, float fov, float width, float height)
{
  u = (u - 0.5f) * width;
  v = (v - 0.5f) * height;

  float r = sqrtf(u * u + v * v);
  float r2 = r * r;
  float4 rr = make_float4(r, r2, r2 * r, r2 * r2);
  float theta = -(coeff0 + dot(coeffs, rr));

  if (fabsf(theta) > 0.5f * fov)
    return zero_float3();

  float phi = safe_acosf((r != 0.0f) ? u / r : 0.0f);

  if (v < 0.0f)
    phi = -phi;

  return make_float3(cosf(theta), -cosf(phi) * sinf(theta), sinf(phi) * sinf(theta));
}

ccl_device float2 direction_to_fisheye_lens_polynomial(
    float3 dir, float coeff0, float4 coeffs, float width, float height)
{
  const float theta = -safe_acosf(dir.x);

  /* Initialize r with the closed-form solution for the special case
   * coeffs.y = coeffs.z = coeffs.w = 0 */
  float r = (theta - coeff0) / coeffs.x;

  const float4 diff_coeffs = make_float4(1.0f, 2.0f, 3.0f, 4.0f) * coeffs;

  for (int i = 0; i < 20; i++) {
    /*  Newton's method for finding roots
     *
     *  Given is the result theta = distortion_model(r),
     *  we need to find r.
     *  Let F(r) := theta - distortion_model(r).
     *  Then F(r) = 0 <=> distortion_model(r) = theta
     *  Therefore we apply Newton's method for finding a root of F(r).
     *  Newton step for the function F:
     *  r_n+1 = r_n - F(r_n) / F'(r_n)
     *  The addition in the implementation is due to canceling of signs.
     * \{ */
    const float old_r = r, r2 = r * r;
    const float F_r = theta - (coeff0 + dot(coeffs, make_float4(r, r2, r2 * r, r2 * r2)));
    const float dF_r = dot(diff_coeffs, make_float4(1.0f, r, r2, r2 * r));
    r += F_r / dF_r;

    /* Early termination if the change is below the threshold */
    if (fabsf(r - old_r) < 1e-6f) {
      break;
    }
    /** \} */
  }

  const float2 uv = r * safe_normalize(make_float2(dir.y, dir.z));
  return make_float2(0.5f - uv.x / width, uv.y / height + 0.5f);
}

/* Mirror Ball <-> Cartesion direction */

ccl_device float3 mirrorball_to_direction(float u, float v)
{
  /* point on sphere */
  float3 dir;

  dir.x = 2.0f * u - 1.0f;
  dir.z = 2.0f * v - 1.0f;

  if (dir.x * dir.x + dir.z * dir.z > 1.0f)
    return zero_float3();

  dir.y = -sqrtf(max(1.0f - dir.x * dir.x - dir.z * dir.z, 0.0f));

  /* reflection */
  float3 I = make_float3(0.0f, -1.0f, 0.0f);

  return 2.0f * dot(dir, I) * dir - I;
}

ccl_device float2 direction_to_mirrorball(float3 dir)
{
  /* inverse of mirrorball_to_direction */
  dir.y -= 1.0f;

  float div = 2.0f * sqrtf(max(-0.5f * dir.y, 0.0f));
  if (div > 0.0f)
    dir /= div;

  float u = 0.5f * (dir.x + 1.0f);
  float v = 0.5f * (dir.z + 1.0f);

  return make_float2(u, v);
}

/* Single face of a equiangular cube map projection as described in
 * https://blog.google/products/google-ar-vr/bringing-pixels-front-and-center-vr-video/ */
ccl_device float3 equiangular_cubemap_face_to_direction(float u, float v)
{
  u = tanf((0.5f - u) * M_PI_2_F);
  v = tanf((v - 0.5f) * M_PI_2_F);

  return normalize(make_float3(1.0f, u, v));
}

ccl_device float2 direction_to_equiangular_cubemap_face(float3 dir)
{
  float u = 0.5f - atan2f(dir.y, dir.x) * 2.0f / M_PI_F;
  float v = atan2f(dir.z, dir.x) * 2.0f / M_PI_F + 0.5f;

  return make_float2(u, v);
}

ccl_device_inline float3 panorama_to_direction(ccl_constant KernelCamera *cam, float u, float v)
{
  switch (cam->panorama_type) {
    case PANORAMA_EQUIRECTANGULAR:
      return equirectangular_range_to_direction(u, v, cam->equirectangular_range);
    case PANORAMA_EQUIANGULAR_CUBEMAP_FACE:
      return equiangular_cubemap_face_to_direction(u, v);
    case PANORAMA_MIRRORBALL:
      return mirrorball_to_direction(u, v);
    case PANORAMA_FISHEYE_EQUIDISTANT:
      return fisheye_to_direction(u, v, cam->fisheye_fov);
    case PANORAMA_FISHEYE_624:
      return fisheye_radtanthinprism_to_direction(
          u, v, cam->width, cam->height, cam->fisheye624_params);
    case PANORAMA_FISHEYE_LENS_POLYNOMIAL:
      return fisheye_lens_polynomial_to_direction(u,
                                                  v,
                                                  cam->fisheye_lens_polynomial_bias,
                                                  cam->fisheye_lens_polynomial_coefficients,
                                                  cam->fisheye_fov,
                                                  cam->sensorwidth,
                                                  cam->sensorheight);
    case PANORAMA_CENTRAL_CYLINDRICAL:
      return central_cylindrical_to_direction(u, v, cam->central_cylindrical_range);
    case PANORAMA_FISHEYE_EQUISOLID:
    default:
      return fisheye_equisolid_to_direction(
          u, v, cam->fisheye_lens, cam->fisheye_fov, cam->sensorwidth, cam->sensorheight);
  }
}

ccl_device_inline float2 direction_to_panorama(ccl_constant KernelCamera *cam, float3 dir)
{
  switch (cam->panorama_type) {
    case PANORAMA_EQUIRECTANGULAR:
      return direction_to_equirectangular_range(dir, cam->equirectangular_range);
    case PANORAMA_EQUIANGULAR_CUBEMAP_FACE:
      return direction_to_equiangular_cubemap_face(dir);
    case PANORAMA_MIRRORBALL:
      return direction_to_mirrorball(dir);
    case PANORAMA_FISHEYE_EQUIDISTANT:
      return direction_to_fisheye(dir, cam->fisheye_fov);
    case PANORAMA_FISHEYE_LENS_POLYNOMIAL:
      return direction_to_fisheye_lens_polynomial(dir,
                                                  cam->fisheye_lens_polynomial_bias,
                                                  cam->fisheye_lens_polynomial_coefficients,
                                                  cam->sensorwidth,
                                                  cam->sensorheight);
    case PANORAMA_CENTRAL_CYLINDRICAL:
      return direction_to_central_cylindrical(dir, cam->central_cylindrical_range);
    case PANORAMA_FISHEYE_EQUISOLID:
    default:
      return direction_to_fisheye_equisolid(
          dir, cam->fisheye_lens, cam->sensorwidth, cam->sensorheight);
  }
}

ccl_device_inline void spherical_stereo_transform(ccl_constant KernelCamera *cam,
                                                  ccl_private float3 *P,
                                                  ccl_private float3 *D)
{
  float interocular_offset = cam->interocular_offset;

  /* Interocular offset of zero means either non stereo, or stereo without
   * spherical stereo. */
  kernel_assert(interocular_offset != 0.0f);

  if (cam->pole_merge_angle_to > 0.0f) {
    const float pole_merge_angle_from = cam->pole_merge_angle_from,
                pole_merge_angle_to = cam->pole_merge_angle_to;
    float altitude = fabsf(safe_asinf((*D).z));
    if (altitude > pole_merge_angle_to) {
      interocular_offset = 0.0f;
    }
    else if (altitude > pole_merge_angle_from) {
      float fac = (altitude - pole_merge_angle_from) /
                  (pole_merge_angle_to - pole_merge_angle_from);
      float fade = cosf(fac * M_PI_2_F);
      interocular_offset *= fade;
    }
  }

  float3 up = make_float3(0.0f, 0.0f, 1.0f);
  float3 side = normalize(cross(*D, up));
  float3 stereo_offset = side * interocular_offset;

  *P += stereo_offset;

  /* Convergence distance is FLT_MAX in the case of parallel convergence mode,
   * no need to modify direction in this case either. */
  const float convergence_distance = cam->convergence_distance;

  if (convergence_distance != FLT_MAX) {
    float3 screen_offset = convergence_distance * (*D);
    *D = normalize(screen_offset - stereo_offset);
  }
}

CCL_NAMESPACE_END
