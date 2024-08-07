
#include <cmath>
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

// Creates a 1D quasirandom sequence, see
// https://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
class QuasiRandom {
  double const offset = 2.0 / (std::sqrt(5.0) + 1.0);
  double value = 0.5;

 public:
  double next()
  {
    value = std::fmod(value + offset, 1.0);
    return value;
  }

  float nextf()
  {
    return float(next());
  }
};

struct MaxError {
 public:
  float max = 0.0f;

  void push(float const val)
  {
    max = std::max(max, val);
  }

  void push(float const a, float const b)
  {
    push(std::abs(a - b));
  }
};

const float rad60 = M_PI_F / 3.0f;
const float cos60 = 0.5f;
const float sin60 = M_SQRT3_F / 2.0f;
const float tan60 = M_SQRT3_F;

const float rad30 = M_PI_F / 6.0f;
const float cos30 = M_SQRT3_F / 2.0f;
const float sin30 = 0.5f;
const float tan30 = M_SQRT3_F / 3.0f;

const float rad45 = M_PI_4F;
const float cos45 = M_SQRT1_2F;
const float sin45 = M_SQRT1_2F;
const float tan45 = 1.0f;

//*
ccl_device_inline std::ostream &operator<<(std::ostream &out, const float4 val)
{
  out << "(" << val.x << ", " << val.y << ", " << val.z << ", " << val.w << ")";
  return out;
}

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

      float const solution_error_squared = tangential_thinprism_error_squared(
          solved, tgt, tangential, thin_prism);
      ASSERT_LT(solution_error_squared, 1e-12) << prefix;
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
 * the original angle and the solution found by solve_radial.
 *
 * @param _k
 * @param fov_deg
 * @param prefix
 */
void test_radial_solver(float const *const radial, float const fov_deg, std::string const &prefix)
{
  size_t const num_samples = 1'000;
  double error_sum = 0;
  for (size_t ii = 0; ii <= num_samples; ++ii) {
    double const angle_rad = deg2rad(ii * 0.5f * fov_deg / num_samples);
    double const angle_tgt = radial_forward(angle_rad, radial);
    double const solution = solve_radial(angle_tgt, radial);
    EXPECT_NEAR(solution, angle_rad, 1e-6) << prefix;
    error_sum += std::abs(solution - angle_rad);
  }
  double const mean_error = error_sum / num_samples;
  EXPECT_LT(mean_error, 2e-8) << prefix;
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

ccl_device_inline float angle_to_noncentrality(float const theta, float3 const params)
{
  float const t2 = theta * theta;
  float const t4 = t2 * t2;
  return dot(params, make_float3(t2, t4, t2 * t4));
}

ccl_device_inline float angle_to_noncentrality_derivative(float const theta, float3 const params)
{
  float const t2 = theta * theta;
  float const t3 = t2 * theta;
  return dot(params, make_float3(2.0f * theta, 4.0f * t3, 6.0f * t2 * t3));
}

ccl_device_inline float point_to_noncentrality(float3 const point, float3 const params)
{
  float const length = len(point);
  if (length < 1e-6f) {
    return 0.0f;
  }
  float const theta = acosf(point.z / length);
  return angle_to_noncentrality(theta, params);
}

ccl_device_inline float solve_noncentrality(float3 const point, float3 const params)
{

  float const r2 = sqr(point.x) + sqr(point.y);
  if (fabsf(r2 < 1e-12f)) {
    return 0.0f;
  }
  float const r = sqrtf(r2);

  float dz = point_to_noncentrality(point, params);

  for (size_t ii = 0; ii < 20; ++ii) {
    float const corrected_z = point.z - dz;
    float const theta = atan2f(r, corrected_z);
    assert(fabsf(theta) >= 0.0f);

    // Derivative of the angle as a function of the corrected z.
    float const a_dz = r / (r2 + corrected_z * corrected_z);

    // Helper-function for Newton's method:
    // F(dz) := M(a(dz)) - dz
    // where M is the noncentrality model and dz is the
    // z-offset computed in the previous iteration.
    float const F = angle_to_noncentrality(theta, params) - dz;

    // First derivative of F:
    // F_dz = M'(a(dz))a'(dz) - 1
    float const F_dz = angle_to_noncentrality_derivative(theta, params) * a_dz - 1.0f;
    float const old_dz = dz;
    dz -= F / F_dz;
    if (fabsf(dz - old_dz) < 1e-6f) {
      break;
    }
  }

  return dz;
}

TEST(KernelCamera, Cam624nc_noncentrality)
{
  int const num_angles = 1000;
  float const max_angle_rad = M_PI_F;

  QuasiRandom rng;
  float max_error = 0;

  float3 all_params[] = {
      make_float3(+1.0f, +0.2f, +0.1f),
      make_float3(-1.0f, -0.2f, -0.1f),

      make_float3(+1.0f, 0.0f, 0.0f),
      make_float3(-1.0f, 0.0f, 0.0f),

      make_float3(0.0f, +0.2f, 0.0f),
      make_float3(0.0f, -0.2f, 0.0f),

      make_float3(0.0f, 0.0f, +0.1f),
      make_float3(0.0f, 0.0f, -0.1f),
  };

  for (float3 const params : all_params) {
    for (int ii = 0; ii < num_angles; ++ii) {
      float const theta = (float(ii) * max_angle_rad) / num_angles;
      float const phi = rng.nextf() * 2.0f * M_PI_F;
      float const z_offset = angle_to_noncentrality(theta, params);
      float3 const direction{cosf(phi) * sinf(theta), sinf(phi) * sinf(theta), cosf(theta)};
      float3 const origin{0.0f, 0.0f, z_offset};
      float const distance = std::abs(z_offset) + 100.0f + 10.0f * (ii % 10);

      float3 const point = origin + distance * direction;
      float const recovered_z_offset = solve_noncentrality(point, params);
      EXPECT_NEAR(recovered_z_offset, z_offset, 3e-5)
          << "params:    " << params << std::endl
          << "ii:        " << ii << std::endl
          << "theta:     " << theta << std::endl
          << "phi:       " << phi << std::endl
          << "z_offset:  " << z_offset << std::endl
          << "direction: " << direction << std::endl
          << "origin:    " << origin << std::endl
          << "distance:  " << distance << std::endl
          << "point:     " << point << std::endl
          << "recovered_z_offset: " << recovered_z_offset << std::endl;

      max_error = std::max(max_error, std::abs(z_offset - recovered_z_offset));
    }
  }

  if (HasFailure()) {
    std::cout << "Maximum z-error: " << max_error << std::endl;
  }
}

enum BaseProjectionType {
  RECTILINEAR = 0,
  EQUIDISTANT = 1,
  STEREOGRAPHIC = 2,
  EQUISOLID = 3,
  FISHEYE_ORTHOGRAPHIC = 4,
  BASE_PROJECTION_NUM_TYPES,
};

std::ostream &operator<<(std::ostream &out, BaseProjectionType const type)
{
  switch (type) {
    case RECTILINEAR:
      out << "RECTILINEAR";
      return out;
    case EQUIDISTANT:
      out << "EQUIDISTANT";
      return out;
    case STEREOGRAPHIC:
      out << "STEREOGRAPHIC";
      return out;
    case EQUISOLID:
      out << "EQUISOLID";
      return out;
    case FISHEYE_ORTHOGRAPHIC:
      out << "FISHEYE_ORTHOGRAPHIC";
      return out;
  }
  out << "UNKNOWN";
  return out;
}

ccl_device_inline float invert_projection_type(BaseProjectionType const type, float const theta)
{
  switch (type) {
    case RECTILINEAR:
      return atanf(theta);
    case EQUIDISTANT:
      return theta;
    case STEREOGRAPHIC:
      return 2.0f * atanf(theta / 2.0f);
    case EQUISOLID:
      return 2.0f * asinf(theta / 2.0f);
    case FISHEYE_ORTHOGRAPHIC:
      return asinf(theta);
  }
  return 0.0f;
}

ccl_device_inline float apply_projection_type(BaseProjectionType const type, float const theta)
{
  switch (type) {
    case RECTILINEAR:
      return tanf(theta);
    case EQUIDISTANT:
      return theta;
    case STEREOGRAPHIC:
      return 2.0 * tanf(theta / 2.0f);
    case EQUISOLID:
      return 2.0f * sinf(theta / 2.0f);
    case FISHEYE_ORTHOGRAPHIC:
      return sinf(theta);
  }
  return 0.0f;
}

float get_fov_deg_from_proj_type(BaseProjectionType const type)
{
  switch (type) {
    case RECTILINEAR:
      return 160;
    case FISHEYE_ORTHOGRAPHIC:
      return 165;
  }
  return 330;
}

TEST(KernelCamera, Cam624nc_apply_projection_type_roundtrip)
{
  int const num_tests = 1'000;
  float const error_threshold = 5e-7;

  // Manual tests with special values
  EXPECT_NEAR(apply_projection_type(RECTILINEAR, rad30), tan30, error_threshold);
  EXPECT_NEAR(apply_projection_type(EQUIDISTANT, 1.0f), 1.0f, error_threshold);
  EXPECT_NEAR(apply_projection_type(STEREOGRAPHIC, rad60), 2.0f * tan30, error_threshold);
  EXPECT_NEAR(apply_projection_type(EQUISOLID, rad60), 2.0f * sin30, error_threshold);
  EXPECT_NEAR(apply_projection_type(FISHEYE_ORTHOGRAPHIC, rad30), sin30, error_threshold);

  EXPECT_NEAR(invert_projection_type(RECTILINEAR, tan30), rad30, error_threshold);
  EXPECT_NEAR(invert_projection_type(EQUIDISTANT, 1.0f), 1.0f, error_threshold);
  EXPECT_NEAR(invert_projection_type(STEREOGRAPHIC, 2.0f * tan30), rad60, error_threshold);
  EXPECT_NEAR(invert_projection_type(EQUISOLID, 2.0f * sin30), rad60, error_threshold);
  EXPECT_NEAR(invert_projection_type(FISHEYE_ORTHOGRAPHIC, sin30), rad30, error_threshold);

  float max_error = 0;

  for (BaseProjectionType const proj_type :
       {RECTILINEAR, EQUIDISTANT, STEREOGRAPHIC, EQUISOLID, FISHEYE_ORTHOGRAPHIC})
  {
    // Center gets mapped to center no matter the projection type
    EXPECT_NEAR(apply_projection_type(proj_type, 0.0f), 0.0f, 1e-10);
    EXPECT_NEAR(invert_projection_type(proj_type, 0.0f), 0.0f, 1e-10);

    // Round-trip consistency test
    float const fov_deg = get_fov_deg_from_proj_type(proj_type) / 2;
    float const fov_rad = deg2rad(fov_deg);
    for (size_t ii = 0; ii <= num_tests; ++ii) {
      float const angle_orig = (float(ii) * fov_rad) / num_tests;
      float const applied = apply_projection_type(proj_type, angle_orig);
      float const reversed = invert_projection_type(proj_type, applied);
      EXPECT_NEAR(reversed, angle_orig, error_threshold)
          << "Projection type: " << proj_type << std::endl;
      max_error = std::max(max_error, std::abs(reversed - angle_orig));
    }
  }

  if (HasFailure()) {
    std::cout << "Maximum error: " << max_error << std::endl;
  }
}

ccl_device_inline float4 cam624nc_to_direction(float const u,
                                               float const v,
                                               float const width,
                                               float const height,
                                               float const fov,
                                               float const focal,
                                               BaseProjectionType const proj_type,
                                               float const *const radial,
                                               float2 const tangential,
                                               float4 const thin_prism)
{
  float2 point{(u - 0.5f) * width / focal, (v - 0.5f) * height / focal};

  point = solve_tangential_thinprism(point, tangential, thin_prism);

  float theta = len(point);

  if (theta > fov) {
    return zero_float4();
  }

  float const r_rad = solve_radial(invert_projection_type(proj_type, theta), radial);
  if (r_rad > 1e-6 && theta > 1e-6) {
    point *= r_rad / theta;
    theta *= r_rad / theta;
  }

  float const phi_c = theta > 1e-6 ? point.x / theta : 0.0f;
  float const phi_s = theta > 1e-6 ? point.y / theta : 1.0f;

  float const theta_s = sinf(theta);
  float const theta_c = cosf(theta);

  return {theta_c, -phi_c * theta_s, phi_s * theta_s, theta};
}

ccl_device_inline float2 direction_to_cam624nc(float4 const dir,
                                               float const width,
                                               float const height,
                                               float const fov,
                                               float const focal,
                                               BaseProjectionType const proj_type,
                                               float const *const radial,
                                               float2 const tangential,
                                               float4 const thin_prism)
{
  float const theta = -safe_acosf(dir.x);

  float const radius = apply_projection_type(proj_type, radial_forward(theta, radial));

  float2 point = radius * safe_normalize(make_float2(dir.y, dir.z));

  point = focal * tangential_thinprism_forward(point, tangential, thin_prism);

  return {0.5f + point.x / width, 0.5f - point.y / height};
}

TEST(KernelCamera, Cam624nc_cam624nc_to_direction_simple)
{
  const float fov = M_PI_F;

  float max_error_x = 0.0f;
  float max_error_y = 0.0f;
  float max_error_z = 0.0f;
  float max_error_theta = 0.0f;

  float max_error_sensor_x = 0.0f;
  float max_error_sensor_y = 0.0f;

  const std::pair<float2, float4> tests[]{
      /* Center (0°) */
      {make_float2(0.0f, 0.0f), make_float4(1.0f, 0.0f, 0.0f, 0.0f)},

      /* 60° */
      {make_float2(0.0f, +rad60), make_float4(cos60, 0.0f, +sin60, rad60)},
      {make_float2(0.0f, -rad60), make_float4(cos60, 0.0f, -sin60, rad60)},
      {make_float2(+rad60, 0.0f), make_float4(cos60, -sin60, 0.0f, rad60)},
      {make_float2(-rad60, 0.0f), make_float4(cos60, +sin60, 0.0f, rad60)},

      /* 45° */
      {make_float2(0.0f, +rad45), make_float4(cos45, 0.0f, +sin45, rad45)},
      {make_float2(0.0f, -rad45), make_float4(cos45, 0.0f, -sin45, rad45)},
      {make_float2(+rad45, 0.0f), make_float4(cos45, -sin45, 0.0f, rad45)},
      {make_float2(-rad45, 0.0f), make_float4(cos45, +sin45, 0.0f, rad45)},

      {make_float2(+rad45 * M_SQRT1_2F, +rad45 * M_SQRT1_2F),
       make_float4(cos45, -0.5f, +0.5f, rad45)},
      {make_float2(-rad45 * M_SQRT1_2F, +rad45 * M_SQRT1_2F),
       make_float4(cos45, +0.5f, +0.5f, rad45)},
      {make_float2(+rad45 * M_SQRT1_2F, -rad45 * M_SQRT1_2F),
       make_float4(cos45, -0.5f, -0.5f, rad45)},
      {make_float2(-rad45 * M_SQRT1_2F, -rad45 * M_SQRT1_2F),
       make_float4(cos45, +0.5f, -0.5f, rad45)},

      /* 30° */
      {make_float2(0.0f, +rad30), make_float4(cos30, 0.0f, +sin30, rad30)},
      {make_float2(0.0f, -rad30), make_float4(cos30, 0.0f, -sin30, rad30)},
      {make_float2(+rad30, 0.0f), make_float4(cos30, -sin30, 0.0f, rad30)},
      {make_float2(-rad30, 0.0f), make_float4(cos30, +sin30, 0.0f, rad30)},
  };

  for (auto [offset, expected] : tests) {
    const float2 sensor = offset + make_float2(0.5f, 0.5f);
    for (float const scale : {1.0f, 0.5f, 2.0f, 0.25f, 4.0f, 0.125f, 8.0f, 0.0625f, 16.0f}) {
      const float width = 1.0f / scale;
      const float height = 1.0f / scale;
      float const focal = 1.0f / scale;
      /* Trivial case: The coefficients create a perfect equidistant fisheye */
      float const radial[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

      const float4 computed = cam624nc_to_direction(sensor.x,
                                                    sensor.y,
                                                    width,
                                                    height,
                                                    fov,
                                                    focal,
                                                    EQUIDISTANT,
                                                    radial,
                                                    zero_float2(),
                                                    zero_float4());

      max_error_x = std::max(max_error_x, std::abs(expected.x - computed.x));
      EXPECT_NEAR(expected.x, computed.x, 6e-8f)
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << expected << std::endl;
      max_error_y = std::max(max_error_y, std::abs(expected.y - computed.y));
      EXPECT_NEAR(expected.y, computed.y, 6e-8f)
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << expected << std::endl;
      max_error_z = std::max(max_error_z, std::abs(expected.z - computed.z));
      EXPECT_NEAR(expected.z, computed.z, 6e-8f)
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << expected << std::endl;
      max_error_theta = std::max(max_error_theta, std::abs(expected.w - computed.w));
      EXPECT_NEAR(expected.w, computed.w, 2e-7f)
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << expected << std::endl;

      // Verify that cam624nc_to_expected returns all zeroes if the point is outside
      // the configured field of view.

      const float4 computed_outside_fov = cam624nc_to_direction(sensor.x,
                                                                sensor.y,
                                                                width,
                                                                height,
                                                                expected.w - 1e-6,
                                                                focal,
                                                                EQUIDISTANT,
                                                                radial,
                                                                zero_float2(),
                                                                zero_float4());

      EXPECT_NEAR(computed_outside_fov.x, 0.0f, 1e-10f)
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << zero_float4() << std::endl;
      EXPECT_NEAR(computed_outside_fov.y, 0.0f, 1e-10f)
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << zero_float4() << std::endl;
      EXPECT_NEAR(computed_outside_fov.z, 0.0f, 1e-10f)
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << zero_float4() << std::endl;
      EXPECT_NEAR(computed_outside_fov.w, 0.0f, 1e-10f)
          << "sensor: (" << sensor.x << ", " << sensor.y << ")" << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << zero_float4() << std::endl;

      // Check round-trip consistency

      const float2 round_trip = direction_to_cam624nc(
          expected, width, height, fov, focal, EQUIDISTANT, radial, zero_float2(), zero_float4());

      max_error_sensor_x = std::max(max_error_sensor_x, std::abs(round_trip.x - sensor.x));
      EXPECT_NEAR(round_trip.x, sensor.x, 2e-7f) << "sensor: " << sensor << std::endl
                                                 << "scale: " << scale << std::endl
                                                 << "round_trip: " << round_trip << std::endl;

      max_error_sensor_y = std::max(max_error_sensor_y, std::abs(round_trip.y - sensor.y));
      EXPECT_NEAR(round_trip.y, sensor.y, 2e-7f) << "sensor: " << sensor << std::endl
                                                 << "scale: " << scale << std::endl
                                                 << "round_trip: " << round_trip << std::endl;
    }
  }

  if (HasFailure()) {
    std::cout << "Maximum error in x: " << max_error_x << std::endl;
    std::cout << "Maximum error in y: " << max_error_y << std::endl;
    std::cout << "Maximum error in z: " << max_error_z << std::endl;
    std::cout << "Maximum error in theta: " << max_error_theta << std::endl;

    std::cout << "Maximum error in sensor_x: " << max_error_sensor_x << std::endl;
    std::cout << "Maximum error in sensor_y: " << max_error_sensor_y << std::endl;
  }
}

/**
 * @brief test_cam624nc_roundtrip tests the correctness of cam624nc_to_direction using a given
 * set of distortion parameters. It has assertions for the difference between
 * the original angle and the solution found by cam624nc_to_direction.
 *
 * @param _k
 * @param fov_deg
 * @param prefix
 */
void test_cam624nc_roundtrip(float const *const radial,
                             float2 const tangential,
                             float4 const thin_prism,
                             float const fov_deg,
                             std::string const &prefix,
                             float const error_threshold)
{
  float const width = 1.0f;
  float const height = 1.0f;
  float const focal = 1.0f;
  float const fov = M_PI_F;
  size_t const num_samples = 10;
  double error_sum = 0;
  QuasiRandom rng;
  MaxError error_x;
  MaxError error_y;
  MaxError error_z;
  MaxError error_theta;
  MaxError error_sensor_x;
  MaxError error_sensor_y;
  for (size_t ii = 0; ii <= num_samples; ++ii) {
    float const theta = deg2rad(ii * 0.5f * fov_deg / num_samples);
    float const phi = rng.nextf() * 2.0f * M_PI_F;
    float4 const expected_dir{
        cosf(theta), -cosf(phi) * sinf(theta), sinf(phi) * sinf(theta), theta};
    float2 const sensor = direction_to_cam624nc(
        expected_dir, width, height, fov, focal, EQUIDISTANT, radial, tangential, thin_prism);
    float4 const recomputed_dir = cam624nc_to_direction(sensor.x,
                                                        sensor.y,
                                                        width,
                                                        height,
                                                        fov,
                                                        focal,
                                                        EQUIDISTANT,
                                                        radial,
                                                        tangential,
                                                        thin_prism);

    error_x.push(expected_dir.x, recomputed_dir.x);
    EXPECT_NEAR(expected_dir.x, recomputed_dir.x, error_threshold)
        << "theta: " << theta << std::endl
        << "phi: " << phi << std::endl
        << "expected_dir: " << expected_dir << std::endl
        << "sensor: " << sensor << std::endl
        << "recomputed_dir: " << recomputed_dir << std::endl
        << "width: " << width << std::endl
        << "height: " << height << std::endl
        << "fov: " << fov << std::endl
        << "focal: " << focal << std::endl
        << "prefix: " << prefix << std::endl;
    error_y.push(expected_dir.y, recomputed_dir.y);
    EXPECT_NEAR(expected_dir.y, recomputed_dir.y, error_threshold)
        << "theta: " << theta << std::endl
        << "phi: " << phi << std::endl
        << "expected_dir: " << expected_dir << std::endl
        << "sensor: " << sensor << std::endl
        << "recomputed_dir: " << recomputed_dir << std::endl
        << "width: " << width << std::endl
        << "height: " << height << std::endl
        << "fov: " << fov << std::endl
        << "focal: " << focal << std::endl
        << "prefix: " << prefix << std::endl;
    error_z.push(expected_dir.z, recomputed_dir.z);
    EXPECT_NEAR(expected_dir.z, recomputed_dir.z, error_threshold)
        << "theta: " << theta << std::endl
        << "phi: " << phi << std::endl
        << "expected_dir: " << expected_dir << std::endl
        << "sensor: " << sensor << std::endl
        << "recomputed_dir: " << recomputed_dir << std::endl
        << "width: " << width << std::endl
        << "height: " << height << std::endl
        << "fov: " << fov << std::endl
        << "focal: " << focal << std::endl
        << "prefix: " << prefix << std::endl;
    error_theta.push(expected_dir.w, recomputed_dir.w);
    EXPECT_NEAR(expected_dir.w, recomputed_dir.w, error_threshold)
        << "theta: " << theta << std::endl
        << "phi: " << phi << std::endl
        << "expected_dir: " << expected_dir << std::endl
        << "sensor: " << sensor << std::endl
        << "recomputed_dir: " << recomputed_dir << std::endl
        << "width: " << width << std::endl
        << "height: " << height << std::endl
        << "fov: " << fov << std::endl
        << "focal: " << focal << std::endl
        << "prefix: " << prefix << std::endl;

    float2 const recomputed_sensor = direction_to_cam624nc(
        recomputed_dir, width, height, fov, focal, EQUIDISTANT, radial, tangential, thin_prism);
    error_sensor_x.push(sensor.x, recomputed_sensor.x);
    EXPECT_NEAR(sensor.x, recomputed_sensor.x, error_threshold / 100.0f)
        << "theta: " << theta << std::endl
        << "phi: " << phi << std::endl
        << "expected_dir: " << expected_dir << std::endl
        << "recomputed_dir: " << recomputed_dir << std::endl
        << "sensor: " << sensor << std::endl
        << "recomputed_sensor: " << recomputed_sensor << std::endl
        << "width: " << width << std::endl
        << "height: " << height << std::endl
        << "fov: " << fov << std::endl
        << "focal: " << focal << std::endl
        << "prefix: " << prefix << std::endl;
    error_sensor_y.push(sensor.y, recomputed_sensor.y);
    EXPECT_NEAR(sensor.y, recomputed_sensor.y, error_threshold / 100.0f)
        << "theta: " << theta << std::endl
        << "phi: " << phi << std::endl
        << "expected_dir: " << expected_dir << std::endl
        << "recomputed_dir: " << recomputed_dir << std::endl
        << "sensor: " << sensor << std::endl
        << "recomputed_sensor: " << recomputed_sensor << std::endl
        << "width: " << width << std::endl
        << "height: " << height << std::endl
        << "fov: " << fov << std::endl
        << "focal: " << focal << std::endl
        << "prefix: " << prefix << std::endl;

  }
  double const mean_error = error_sum / num_samples;
  EXPECT_LT(mean_error, 2e-8) << prefix;

  if (::testing::Test::HasFailure()) {
    std::cout << "Maximum error in x for " << prefix << ": " << error_x.max << std::endl;
    std::cout << "Maximum error in y for " << prefix << ": " << error_y.max << std::endl;
    std::cout << "Maximum error in z for " << prefix << ": " << error_z.max << std::endl;
    std::cout << "Maximum error in theta for " << prefix << ": " << error_theta.max << std::endl;
    std::cout << "Maximum error in sensor_x for " << prefix << ": " << error_sensor_x.max << std::endl;
    std::cout << "Maximum error in sensor_y for " << prefix << ": " << error_sensor_y.max << std::endl;
  }
}

TEST(KernelCamera, Cam624nc_cam624nc_to_direction_round_trip)
{
  float2 const p{-1.7905108189099640e-04, 3.6947302643007590e-06};
  // TODO: Replace these values by values obtained from real calibrations
  float4 const s{-2e-4, 1e-4, 3e-4, -5e-4};

  {
    // Testcase from a real calib of a lens which is very non-equidistant:
    float const k[]{-6.3212689067106823e-02f,
                    1.0783254109563612e-02f,
                    -1.5666209452467651e-02f,
                    1.0487251796288639e-02f,
                    -3.8781789116892440e-03f,
                    5.6914433571826422e-04f};

    test_cam624nc_roundtrip(k, p, s, 165.0f, "non-equidistant", 3e-3);
  }

  {
    // Testcase from a real calib of a lens which is almost equidistant.
    float const k[]{6.8925238237090430e-03f,
                    4.9065099682158737e-03f,
                    -5.6645010933102091e-03f,
                    3.5255596948621580e-03f,
                    -1.2505361069399053e-03f,
                    1.6815868507166388e-04f};

    test_cam624nc_roundtrip(k, p, s, 170.0f, "almost-equidistant", 4e-3);
  }

  {
    // Testcase from a real calib of some roughly "orthographic fisheye" lens.
    float const k[]{-2.7071250929750468e-01,
                    5.1892349368743274e-01,
                    -1.0625944626790622e+00,
                    1.1393696445669612e+00,
                    -6.2508654084092763e-01,
                    1.4034706020688248e-01};

    test_cam624nc_roundtrip(k, p, s, 122.0f, "orthographic", 3e-4);
  }
}

CCL_NAMESPACE_END
