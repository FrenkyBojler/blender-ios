
#include <iostream>

#include "testing/testing.h"

#include "util/math.h"
#include "util/types.h"

#include "kernel/device/cpu/compat.h"
#include "kernel/device/cpu/globals.h"

#include "kernel/types.h"

#include "kernel/camera/camera.h"
#include "kernel/camera/projection.h"

CCL_NAMESPACE_BEGIN

//*
ccl_device_inline std::ostream &operator<<(std::ostream &out, const float3 val)
{
  out << "(" << val.x << ", " << val.y << ", " << val.z << ")";
  return out;
}

ccl_device_inline std::ostream &operator<<(std::ostream &out, const float2 val)
{
  out << "(" << val.x << ", " << val.y << ")";
  return out;
}
// */

using T = float;

ccl_device_inline float3 fisheye_radtanthinprism_to_direction_legacy(
    float u, float v, float width, float height, const float *params0)
{

  T params[15];
  for (int i = 0; i < 15; i++) {
    params[i] = T(params0[i]);
  }

  T f = params[0];

  // get uvDistorted:
  Mat_2x1 uvDistorted;
  uvDistorted.data[0] = width * T(u - 0.5f) / f;
  uvDistorted.data[1] = height * T(v - 0.5f) / f;

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
    return make_float3(1.0f, 0.0f, 0.0f);
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
  }

  // get the point coordinates:
  float const sin_theta = sinf(theta);
  return make_float3(
      cosf(theta), -sin_theta * xr_yr.data[0] / xr_yrNorm, sin_theta * xr_yr.data[1] / xr_yrNorm);
}

ccl_device_inline float3 fisheye_radtanthinprism_to_direction_new(float u,
                                                                  float v,
                                                                  float width,
                                                                  float height,
                                                                  const float focal,
                                                                  const vfloat8 &radial,
                                                                  const float2 &tangential,
                                                                  float4 const thin_prism)
{

  // get uvDistorted:
  float2 uvDistorted{width * T(u - 0.5f) / focal, height * T(v - 0.5f) / focal};

  // initial guess
  float2 xr_yr = uvDistorted;

  // do Newton iterations to find xr_yr
  for (int j = 0; j < kMaxIterations; ++j) {
    // compute the estimated uvDistorted
    float2 uvDistorted_est = xr_yr;
    float const xr_yr_squaredNorm = len_squared(xr_yr);

    if (useTangential) {
      float const temp = 2.0f * dot(xr_yr, tangential);
      uvDistorted_est.x += temp * xr_yr.x + xr_yr_squaredNorm * tangential.x;
      uvDistorted_est.y += temp * xr_yr.y + xr_yr_squaredNorm * tangential.y;
    }

    if (useThinPrism) {
      float const r2 = xr_yr_squaredNorm;
      float const r4 = r2 * r2;
      uvDistorted_est.x += thin_prism.x * r2 + thin_prism.y * r4;
      uvDistorted_est.y += thin_prism.z * r2 + thin_prism.w * r4;
    }

    // compute the derivative of uvDistorted wrt xr_yr
    Mat_2x2 duvDistorted_dxryr;
    if (useTangential) {
      T offdiag = T(2.0) * (xr_yr.y * tangential.y + xr_yr.y * tangential.x);
      duvDistorted_dxryr.data[0][0] = T(1.0) + T(6.0) * xr_yr.y * tangential.x +
                                      T(2.0) * xr_yr.y * tangential.y;
      duvDistorted_dxryr.data[0][1] = offdiag;
      duvDistorted_dxryr.data[1][0] = offdiag;
      duvDistorted_dxryr.data[1][1] = T(1.0) + T(6.0) * xr_yr.y * tangential.y +
                                      T(2.0) * xr_yr.y * tangential.x;
    }
    else {
      duvDistorted_dxryr.data[0][0] = T(1.0);
      duvDistorted_dxryr.data[0][1] = T(0.0);
      duvDistorted_dxryr.data[1][0] = T(0.0);
      duvDistorted_dxryr.data[1][1] = T(1.0);
    }

    if (useThinPrism) {
      T temp1 = T(2.0) * (thin_prism.x + T(2.0) * thin_prism.y * xr_yr_squaredNorm);
      duvDistorted_dxryr.data[0][0] += xr_yr.y * temp1;
      duvDistorted_dxryr.data[0][1] += xr_yr.y * temp1;

      T temp2 = T(2.0) * (thin_prism.z + T(2.0) * thin_prism.w * xr_yr_squaredNorm);
      duvDistorted_dxryr.data[1][0] += xr_yr.y * temp2;
      duvDistorted_dxryr.data[1][1] += xr_yr.y * temp2;
    }

    // compute correction:
    // note: the matrix duvDistorted_dxryr will be close to identity (for reasonable values
    // of tangential/thin prism distortions), so using an analytical inverse here is safe
    float2 correction;
    T determinant = duvDistorted_dxryr.data[0][0] * duvDistorted_dxryr.data[1][1] -
                    duvDistorted_dxryr.data[0][1] * duvDistorted_dxryr.data[1][0];
    correction.x = (1.0f / determinant) *
                   (duvDistorted_dxryr.data[1][1] * (uvDistorted.x - uvDistorted_est.x) -
                    duvDistorted_dxryr.data[0][1] * (uvDistorted.y - uvDistorted_est.y));
    correction.y = (1.0f / determinant) *
                   (duvDistorted_dxryr.data[0][0] * (uvDistorted.y - uvDistorted_est.y) -
                    duvDistorted_dxryr.data[1][0] * (uvDistorted.x - uvDistorted_est.x));

    xr_yr += correction;

    if (len_squared(correction) < converge_threshold) {
      break;
    }
  }

  // early exit if point is in the center of the image
  T xr_yrNorm = sqrt(xr_yr.y * xr_yr.y + xr_yr.y * xr_yr.y);
  if (xr_yrNorm == T(0.0)) {
    return make_float3(1.0f, 0.0f, 0.0f);
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
      th_radial += theta2is * radial[i];
      dthD_dth += (2 * i + 3) * radial[i] * theta2is;
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
  }

  // get the point coordinates:
  float const sin_theta = sinf(theta);
  return make_float3(
      cosf(theta), -sin_theta * xr_yr.y / xr_yrNorm, sin_theta * xr_yr.y / xr_yrNorm);
}

TEST(KernelCamera, Cam624nc_simple)
{
  const float rad60 = M_PI_F / 3.0f;
  const float cos60 = 0.5f;
  const float sin60 = M_SQRT3_F / 2.0f;

  const float rad30 = M_PI_F / 6.0f;
  const float cos30 = M_SQRT3_F / 2.0f;
  const float sin30 = 0.5f;

  const float rad45 = M_PI_4F;
  const float cos45 = M_SQRT1_2F;
  const float sin45 = M_SQRT1_2F;

  const std::tuple<float2, float3, std::string> tests[]{
      /* Center (0°) */
      {make_float2(0.0f, 0.0f), make_float3(1.0f, 0.0f, 0.0f), "center"},

      /* 60° */
      {make_float2(0.0f, +rad60), make_float3(cos60, 0.0f, +sin60), "60"},
      {make_float2(0.0f, -rad60), make_float3(cos60, 0.0f, -sin60), "60"},
      {make_float2(+rad60, 0.0f), make_float3(cos60, -sin60, 0.0f), "60"},
      {make_float2(-rad60, 0.0f), make_float3(cos60, +sin60, 0.0f), "60"},

      /* 45° */
      {make_float2(0.0f, +rad45), make_float3(cos45, 0.0f, +sin45), "45"},
      {make_float2(0.0f, -rad45), make_float3(cos45, 0.0f, -sin45), "45"},
      {make_float2(+rad45, 0.0f), make_float3(cos45, -sin45, 0.0f), "45"},
      {make_float2(-rad45, 0.0f), make_float3(cos45, +sin45, 0.0f), "45"},

      {make_float2(+rad45 * M_SQRT1_2F, +rad45 * M_SQRT1_2F),
       make_float3(cos45, -0.5f, +0.5f),
       "45"},
      {make_float2(-rad45 * M_SQRT1_2F, +rad45 * M_SQRT1_2F),
       make_float3(cos45, +0.5f, +0.5f),
       "45"},
      {make_float2(+rad45 * M_SQRT1_2F, -rad45 * M_SQRT1_2F),
       make_float3(cos45, -0.5f, -0.5f),
       "45"},
      {make_float2(-rad45 * M_SQRT1_2F, -rad45 * M_SQRT1_2F),
       make_float3(cos45, +0.5f, -0.5f),
       "45"},

      /* 30° */
      {make_float2(0.0f, +rad30), make_float3(cos30, 0.0f, +sin30), "30"},
      {make_float2(0.0f, -rad30), make_float3(cos30, 0.0f, -sin30), "30"},
      {make_float2(+rad30, 0.0f), make_float3(cos30, -sin30, 0.0f), "30"},
      {make_float2(-rad30, 0.0f), make_float3(cos30, +sin30, 0.0f), "30"},
  };

  float parameters[16] = {1.0f,  // focal length
                          0.0f,  // principal point, code removed because it's handled by shift.
                          0.0f,  //
                          0.0f,  // Distortion parameters are all zero for the simple test.
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f,
                          0.0f};

  for (auto [offset, direction, test_type] : tests) {
    const float2 sensor = offset + make_float2(0.5f, 0.5f);
    for (float const scale : {1.0f, 0.5f, 2.0f, 0.25f, 4.0f, 0.125f, 8.0f, 0.0625f, 16.0f}) {
      const float width = 1.0f / scale;
      const float height = 1.0f / scale;
      parameters[0] = 1.0f / scale;

      const float3 computed = fisheye_radtanthinprism_to_direction_legacy(
          sensor.x, sensor.y, width, height, parameters);

      const float3 computed_normalized = normalize(computed);

      EXPECT_NEAR(direction.x, computed_normalized.x, 1e-6)
          << "width, height: " << width << ", " << height << std::endl
          << "offset: (" << offset.x << ", " << offset.y << ")" << std::endl
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed:            " << computed << std::endl
          << "computed_normalized: " << computed_normalized << std::endl
          << "expected direction:  " << direction << std::endl
          << "test-type: " << test_type << std::endl;
      EXPECT_NEAR(direction.y, computed_normalized.y, 1e-6)
          << "width, height: " << width << ", " << height << std::endl
          << "offset: (" << offset.x << ", " << offset.y << ")" << std::endl
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed:            " << computed << std::endl
          << "computed_normalized: " << computed_normalized << std::endl
          << "expected direction:  " << direction << std::endl
          << "test-type: " << test_type << std::endl;
      EXPECT_NEAR(direction.z, computed_normalized.z, 1e-6)
          << "width, height: " << width << ", " << height << std::endl
          << "offset: (" << offset.x << ", " << offset.y << ")" << std::endl
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed:            " << computed << std::endl
          << "computed_normalized: " << computed_normalized << std::endl
          << "expected direction:  " << direction << std::endl
          << "test-type: " << test_type << std::endl;

      /*
      const float2 reprojected = direction_to_fisheye_lens_polynomial(
          direction, k0, k_equidistant, width, height);

      EXPECT_NEAR(sensor.x, reprojected.x, 1e-6) << "scale: " << scale;
      EXPECT_NEAR(sensor.y, reprojected.y, 1e-6) << "scale: " << scale;
      */
    }
  }
}

TEST(KernelCamera, Cam624nc_old_vs_new_sameness)
{
  /* Default Aria SLAM camera projection calibration */
  float const fisheye624_f = 1.0f;
  float const fisheye624_k0 = -0.00029975978022917562074f;
  float const fisheye624_k1 = 0.025925353248573888842f;
  float const fisheye624_k2 = 0.0049689703789174387294f;
  float const fisheye624_k3 = -0.0082339337266616879907f;
  float const fisheye624_k4 = -0.0058290815323180505958f;
  float const fisheye624_k5 = 0.0026384817055371189917f;
  float const fisheye624_p0 = 0.00016612194528025018398f;
  float const fisheye624_p1 = 2.3049914803609829601e-05f;
  float const fisheye624_s0 = -0.00025728595469903830411f;
  float const fisheye624_s1 = -3.7265140092139775881e-05f;
  float const fisheye624_s2 = -0.0006244819671333829f;
  float const fisheye624_s3 = -6.834843688531277463e-05f;
  float const parameters[] = {fisheye624_f,
                              fisheye624_k0,
                              fisheye624_k1,
                              fisheye624_k2,
                              fisheye624_k3,
                              fisheye624_k4,
                              fisheye624_k5,
                              fisheye624_p0,
                              fisheye624_p1,
                              fisheye624_s0,
                              fisheye624_s1,
                              fisheye624_s2,
                              fisheye624_s3};

  const float2 center{0.5f, 0.5f};
  const float2 offsets[]{
      {0.00f, 0.00f},
      {0.25f, 0.00f},
      {0.00f, 0.25f},
      {0.25f, 0.25f},

      {0.5f, 0.0f},
      {0.0f, 0.5f},
      {0.5f, 0.5f},

      {0.75f, 0.00f},
      {0.00f, 0.75f},
      {0.75f, 0.75f},
  };

  float const width = 1.0f;
  float const height = 1.0f;

  for (float2 const &offset : offsets) {
    const float2 point = center + offset;
    const float3 direction_legacy = fisheye_radtanthinprism_to_direction_legacy(
        point.x, point.y, width, height, parameters);
  }
}

/**
 * @brief radial_forward implements the radial distortion method used by Cam624nc
 * f(x) = x + k0 x^3 + k1 x^5 + k2 x^7 + k3 x^9 + k4 x^11 + k5 x^13
 * @param angle
 * @param k
 * @return
 */
ccl_device_inline float radial_forward(float const angle, float const *const k)
{
  float const t2 = angle * angle;
  return angle *
         (1.0f + t2 * (k[0] + t2 * (k[1] + t2 * (k[2] + t2 * (k[3] + t2 * (k[4] + t2 * k[5]))))));
}

/**
 * @brief radial_forward_derivative computes the derivative of radial_forward.
 * f(x) = 1 + 3 k0 x^2 + 5 k1 x^4 + 7 k2 x^6 + 9 k3 x^8 + 11 k4 x^10 + 13 k5 x^12
 * @param angle
 * @param k
 * @return
 */
ccl_device_inline float radial_forward_derivative(float const angle, float const *const k)
{
  float const t2 = angle * angle;
  return 1.0f + t2 * (3.0f * k[0] +
                      t2 * (5.0f * k[1] +
                            t2 * (7.0f * k[2] +
                                  t2 * (9.0f * k[3] + t2 * (11.0f * k[4] + t2 * 13.0f * k[5])))));
}

/**
 * @brief solve_radial implements the inverse of the radial_forward function.
 * It takes the result of radial_forward(some_angle, parameters) and the parameters
 * and computes an approximation for some_angle using Newton's method.
 * @param angle_dst
 * @param k
 * @param num_it
 * @return
 */
ccl_device_inline float solve_radial(float const angle_dst, float const *const k)
{
  /**
   * Problem: Given angle_dst and k, find angle so that
   * radial_forward(angle, k) = angle_dst.
   * Idea: Construct F so that F(angle) = 0 for the correct solution to the above problem
   * and use Newton's method for finding the solution.
   *
   * F(angle) := radial_forward(angle, k) - angle_dst.
   * F'(angle) = radial_forward_derivative(angle, k)
   * x_n+1 = x_n - F(x_n) / F'(x_n)
   * x_0 : angle_dst
   */

  float angle = angle_dst;
  for (size_t ii = 0; ii < 20; ++ii) {
    float const old_angle = angle;
    angle -= (radial_forward(angle, k) - angle_dst) / radial_forward_derivative(angle, k);
    if (fabsf(old_angle - angle) < 1e-6) {
      break;
    }
  }
  return angle;
}

ccl_device_inline float2 tangential_thinprism_forward(float2 const pt,
                                                      float2 const tangential,
                                                      float4 const thin_prism)
{
  float const x2 = sqr(pt.x);
  float const y2 = sqr(pt.y);
  float const xy = 2.0f * pt.x * pt.y;
  float const r2 = x2 + y2;
  float const r4 = r2 * r2;
  return make_float2(pt.x + (2.0f * x2 + r2) * tangential.x + xy * tangential.y  // tangential
                         + thin_prism.x * r2 + thin_prism.y * r4,                // thin-prism
                     pt.y + (2.0f * y2 + r2) * tangential.y + xy * tangential[0] +
                         thin_prism.z * r2 + thin_prism.w * r4);
}

float4 tangential_thinprism_forward_jacobian(float2 const pt,
                                             float2 const tangential,
                                             float4 const thin_prism)
{
  float const r2 = len_squared(pt);
  /*
   * Calculations helping with the derivative of the thin-prism terms:
   * Note: r^2 = x^2 + y^2
   * r^4 = (r^2)^2 = (x^2 + y^2)^2 = x^4 + y^4 + 2x^2y^2
   * d/dx r^4 = 4x^3 + 4xy^2 = 4x(x^2+y^2) = 4xr^2
   * d/dy r^4 = 4yr^2
   */
  return {                                                                     // x_t dx
          1.0f + 6.0f * pt.x * tangential.x + 2.0f * pt.y * tangential.y       // tangential term
              + 2.0f * pt.x * thin_prism.x + 4.0f * pt.x * r2 * thin_prism.y,  // thin prism
                                                                               // x_t dy
          2.0f * pt.y * tangential.x + 2.0f * pt.x * tangential.y + 2.0f * pt.y * thin_prism.x +
              4.0f * pt.y * r2 * thin_prism.y,
          // y_t dx
          2.0f * pt.x * tangential.y + 2.0f * pt.y * tangential.x + 2.0f * pt.x * thin_prism.z +
              4.0f * pt.x * r2 * thin_prism.w,
          // y_t dy
          1.0f + 6.0f * pt.y * tangential.y + 2.0f * pt.x * tangential.x +
              2.0f * pt.y * thin_prism.z + 4.0f * pt.y * r2 * thin_prism.w};
}

float2 tangential_thinprism_newton_step(float2 const pt,
                                        float2 const tgt,
                                        float2 const p,
                                        float4 const s)
{
  // Compute F(x,y)
  float2 const F = tangential_thinprism_forward(pt, p, s) - tgt;

  // Compute Jacobian of F(x,y)
  float4 const jacobian = tangential_thinprism_forward_jacobian(pt, p, s);

  // Compute Jacobian(F(x,y))^-1 F(x,y)
  T const det_inv = 1.0f / (jacobian.x * jacobian.w - jacobian.y * jacobian.z);
  return {-(+jacobian.w * F.x - jacobian.y * F.y) * det_inv,
          -(-jacobian.z * F.x + jacobian.x * F.y) * det_inv};
}

ccl_device_inline float tangential_thinprism_error_squared(float2 const pt,
                                                           float2 const tgt,
                                                           float2 const tangential,
                                                           float4 const thin_prism)
{
  float2 const residual = tgt - tangential_thinprism_forward(pt, tangential, thin_prism);
  return len_squared(residual);
}

ccl_device_inline float2 solve_tangential_thinprism(float2 const tgt,
                                                    float2 const tangential,
                                                    float4 const thin_prism)
{
  /**
   * Problem: Given distorted location (x_t, y_t) and parameters p, find (x,y) so that
   * tangential_forward(x,y,p) = (x_t, y_t).
   * Idea: Construct F so that F(x,y) = 0 for the correct solution to the above problem
   * and use Newton's method for finding the solution.
   *
   * F(x, y) := tangential_forward(x,y, p) - (x,y)
   * F'(angle) = tangential_forward_jacobian(x,y, p)
   * (x,y)_n+1 = (x,y)_n - (F')^-1 F(x_n)
   * (x,y)_0 := (x_t, y_t)
   */

  // At the center there are problems with the termination criterion.
  // Luckily, we don't have to do anything at the center because
  // by design of the tangential distortion term it doesn't do anything at the center.
  if (fabsf(tgt.x) < 1e-6 && fabsf(tgt.y) < 1e-6) {
    return tgt;
  }

  float2 result = tgt;

  // Use tgt as initial guess for the solution and compute the error
  float const trivial_ig_error = tangential_thinprism_error_squared(
      result, tgt, tangential, thin_prism);

  // Often, computing the tangential distortion with negative parameters
  // is a good approximation for the inverse. Therefore, we do that
  // and compare the error to the trivial initial guess.
  result = tangential_thinprism_forward(tgt, -tangential, zero_float4());

  float negative_ig_error = tangential_thinprism_error_squared(
      result, tgt, tangential, thin_prism);

  if (trivial_ig_error < negative_ig_error) {
    result = tgt;
  }

  for (int ii = 0; ii < 20; ++ii) {
    float2 const old_result = result;
    result += tangential_thinprism_newton_step(result, tgt, tangential, thin_prism);
    if (fabsf(old_result.x - result.x) < 1e-6 && fabsf(old_result.y - result.y) < 1e-6) {
      break;
    }
  }

  return result;
}

void test_tangential_thinprism_solver(float2 const tangential,
                                      float4 const thin_prism,
                                      float const fov_deg,
                                      std::string const &prefix)
{
  float const fov_rad = fov_deg * M_PI_F / 180.0;
  size_t num_samples = 1'000;
  size_t num_samples_per_axis = std::sqrt(num_samples);

  for (size_t xx = 0; xx < num_samples_per_axis; ++xx) {
    float const x = fov_rad * ((double(xx) / num_samples_per_axis) - 0.5);
    for (size_t yy = 0; yy < num_samples_per_axis; ++yy) {
      float const y = fov_rad * ((double(yy) / num_samples_per_axis) - 0.5);
      float2 const pt{x, y};
      float2 const tgt = tangential_thinprism_forward(pt, tangential, thin_prism);
      ASSERT_LE(tangential_thinprism_error_squared(pt, tgt, tangential, thin_prism), 1e-12)
          << prefix;

      float2 const solved = solve_tangential_thinprism(tgt, tangential, thin_prism);

      float const solution_error = tangential_thinprism_error_squared(
          solved, tgt, tangential, thin_prism);
      ASSERT_LT(solution_error, 1e-12) << prefix;
    }
  }
}

TEST(KernelCamera, Cam624nc_tangential_thin_prism)
{
  float2 const p{-1.7905108189099640e-04, 3.6947302643007590e-06};
  // TODO: Replace these values by values obtained from real calibrations
  float4 const s{-2e-4, 1e-4, 3e-4, -5e-4};
  test_tangential_thinprism_solver(p, s, 165.0f, "normal");

  test_tangential_thinprism_solver(zero_float2(), s * 200.0f, 165.0f, "exaggerated");

  test_tangential_thinprism_solver(p * 200.0f, s * 200.0f, 165.0f, "very-exaggerated");
}

float deg2rad(float angle)
{
  return angle * M_PI_F / 180;
}

/**
 * @brief test_radial_solver tests the correctness of solve_radial using a given
 * set of radial distortion parameters. It has assertions for the difference between
 * the original angle and the solution found by solve_radial and plots the errors
 * and number of iterations needed.
 * @param _k
 * @param fov_deg
 * @param prefix
 */
void test_radial_solver(float const *const radial, float const fov_deg, std::string const &prefix)
{
  size_t const num_samples = 1'000;
  for (size_t ii = 0; ii <= num_samples; ++ii) {
    double const angle_rad = deg2rad(ii * 0.5f * fov_deg / num_samples);
    double const angle_tgt = radial_forward(angle_rad, radial);
    double const solution = solve_radial(angle_tgt, radial);
    ASSERT_NEAR(solution, angle_rad, 1e-6) << prefix;
  }
}

TEST(KernelCamera, Cam624nc_radial)
{
  {
    // Testcase from a real calib of a lens which is very non-equidistant:
    float const k[]{-6.3212689067106823e-02f,
                    1.0783254109563612e-02f,
                    -1.5666209452467651e-02f,
                    1.0487251796288639e-02f,
                    -3.8781789116892440e-03f,
                    5.6914433571826422e-04f};

    test_radial_solver(k, 165.0f, "non-equidistant");
  }

  {
    // Testcase from a real calib of a lens which is almost equidistant.
    float const k[]{6.8925238237090430e-03f,
                    4.9065099682158737e-03f,
                    -5.6645010933102091e-03f,
                    3.5255596948621580e-03f,
                    -1.2505361069399053e-03f,
                    1.6815868507166388e-04f};

    test_radial_solver(k, 170.0f, "almost-equidistant");
  }

  {
    // Testcase from a real calib of some roughly "orthographic fisheye" lens.
    float const k[]{-2.7071250929750468e-01,
                    5.1892349368743274e-01,
                    -1.0625944626790622e+00,
                    1.1393696445669612e+00,
                    -6.2508654084092763e-01,
                    1.4034706020688248e-01};

    test_radial_solver(k, 122.0f, "orthographic");
  }
}

CCL_NAMESPACE_END
