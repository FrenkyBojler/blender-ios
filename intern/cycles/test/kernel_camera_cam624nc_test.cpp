
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

TEST(KernelCamera, Cam62nc_simple)
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

TEST(KernelCamera, Cam62nc_old_vs_new)
{
  /* Default Aria SLAM camera projection calibration */
  float const fisheye624_f = 240.96908202503016128f;
  float const fisheye624_cx = 319.30031322283957707f;
  float const fisheye624_cy = 239.70226462142591117f;
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
}

CCL_NAMESPACE_END
