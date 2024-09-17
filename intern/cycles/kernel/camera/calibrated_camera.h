#pragma once

#include "util/math.h"
#include "util/types.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline float2 blender2calib(float2 a)
{
  return make_float2(a.x, 1.0f - a.y);
}

ccl_device_inline float2 calib2blender(float2 a)
{
  return blender2calib(a);
}

ccl_device_inline float4 calib2blender(float4 a)
{
  return make_float4(a.z, -a.x, -a.y, a.w);
}

ccl_device_inline float4 blender2calib(float4 a)
{
  return make_float4(-a.y, -a.z, a.x, a.w);
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
    case BASE_PROJECTION_NUM_TYPES:
      return 0.0f;
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
      return 2.0f * tanf(theta / 2.0f);
    case EQUISOLID:
      return 2.0f * sinf(theta / 2.0f);
    case FISHEYE_ORTHOGRAPHIC:
      return sinf(theta);
    case BASE_PROJECTION_NUM_TYPES:
      return 0.0f;
  }
  return 0.0f;
}

/**
 * @brief radial_forward implements the radial distortion method used by the calibrated camera model. *
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
    if (fabsf(old_angle - angle) < 1e-6f) {
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

ccl_device_inline float4 tangential_thinprism_forward_jacobian(float2 const pt,
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
  return make_float4(                                                      // x_t dx
      1.0f + 6.0f * pt.x * tangential.x + 2.0f * pt.y * tangential.y       // tangential term
          + 2.0f * pt.x * thin_prism.x + 4.0f * pt.x * r2 * thin_prism.y,  // thin prism
                                                                           // x_t dy
      2.0f * pt.y * tangential.x + 2.0f * pt.x * tangential.y + 2.0f * pt.y * thin_prism.x +
          4.0f * pt.y * r2 * thin_prism.y,
      // y_t dx
      2.0f * pt.x * tangential.y + 2.0f * pt.y * tangential.x + 2.0f * pt.x * thin_prism.z +
          4.0f * pt.x * r2 * thin_prism.w,
      // y_t dy
      1.0f + 6.0f * pt.y * tangential.y + 2.0f * pt.x * tangential.x + 2.0f * pt.y * thin_prism.z +
          4.0f * pt.y * r2 * thin_prism.w);
}

ccl_device_inline float2 tangential_thinprism_newton_step(float2 const pt,
                                                          float2 const tgt,
                                                          float2 const p,
                                                          float4 const s)
{
  // Compute F(x,y)
  float2 const F = tangential_thinprism_forward(pt, p, s) - tgt;

  // Compute Jacobian of F(x,y)
  float4 const jacobian = tangential_thinprism_forward_jacobian(pt, p, s);

  // Compute Jacobian(F(x,y))^-1 F(x,y)
  float const det_inv = 1.0f / (jacobian.x * jacobian.w - jacobian.y * jacobian.z);
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
  if (fabsf(tgt.x) < 1e-6f && fabsf(tgt.y) < 1e-6f) {
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
    if (fabsf(old_result.x - result.x) < 1e-6f && fabsf(old_result.y - result.y) < 1e-6f) {
      break;
    }
  }

  return result;
}

ccl_device_inline float4 calibrated_cam_to_direction_impl(float2 point,
                                                          float const sensorwidth,
                                                          float const sensorheight,
                                                          float const fov,
                                                          float const focal,
                                                          BaseProjectionType const proj_type,
                                                          float const *const radial,
                                                          float2 const tangential,
                                                          float4 const thin_prism)
{
  point = make_float2((point.x - 0.5f) * sensorwidth / focal,
                      (point.y - 0.5f) * sensorheight / focal);

  point = solve_tangential_thinprism(point, tangential, thin_prism);

  float const sensor_rad = len(point);

  float const theta = solve_radial(invert_projection_type(proj_type, sensor_rad), radial);
  if (theta > 1e-6f && sensor_rad > 1e-6f) {
    point *= theta / sensor_rad;
  }

  if (2.0f * theta > fov) {
    return zero_float4();
  }

  float const phi_c = theta > 1e-6f ? point.x / theta : 1.0f;
  float const phi_s = theta > 1e-6f ? point.y / theta : 0.0f;

  float const theta_s = sinf(theta);
  float const theta_c = cosf(theta);

  return make_float4(phi_c * theta_s, phi_s * theta_s, theta_c, theta);
}

ccl_device_inline float2 direction_to_calibrated_cam_impl(float4 const dir,
                                                          float const width,
                                                          float const height,
                                                          float const focal,
                                                          BaseProjectionType const proj_type,
                                                          float const *const radial,
                                                          float2 const tangential,
                                                          float4 const thin_prism)
{
  float const theta = safe_acosf(dir.z);

  float const radius = apply_projection_type(proj_type, radial_forward(theta, radial));

  float2 point = radius * safe_normalize(make_float2(dir.x, dir.y));

  point = focal * tangential_thinprism_forward(point, tangential, thin_prism);

  return {0.5f + point.x / width, 0.5f + point.y / height};
}

ccl_device_inline float2 direction_to_calibrated_cam(float4 const dir,
                                                     float const width,
                                                     float const height,
                                                     float const focal,
                                                     BaseProjectionType const proj_type,
                                                     float const *const radial,
                                                     float2 const tangential,
                                                     float4 const thin_prism)
{
  float2 const result = direction_to_calibrated_cam_impl(
      blender2calib(dir), width, height, focal, proj_type, radial, tangential, thin_prism);
  return calib2blender(result);
}

ccl_device_inline float4 calibrated_cam_to_direction(float2 sensor,
                                                     float const width,
                                                     float const height,
                                                     float const fov,
                                                     float const focal,
                                                     BaseProjectionType const proj_type,
                                                     float const *const radial,
                                                     float2 const tangential,
                                                     float4 const thin_prism)
{
  float4 const result = calibrated_cam_to_direction_impl(
      blender2calib(sensor), width, height, fov, focal, proj_type, radial, tangential, thin_prism);
  return calib2blender(result);
}

ccl_device_inline float angle_to_noncentrality(float const theta, float4 const params)
{
  float const t2 = theta * theta;
  float const t4 = t2 * t2;
  return dot(params, make_float4(t2, t4, t2 * t4, t4 * t4));
}

ccl_device_inline float angle_to_noncentrality_derivative(float const theta, float4 const params)
{
  float const t2 = theta * theta;
  float const t3 = t2 * theta;
  float const t5 = t2 * t3;
  return dot(params, make_float4(2.0f * theta, 4.0f * t3, 6.0f * t5, 8.0f * t5 * t2));
}

ccl_device_inline float point_to_noncentrality(float3 const point, float4 const params)
{
  float const length = len(point);
  if (length < 1e-6f) {
    return 0.0f;
  }
  float const theta = acosf(point.z / length);
  return angle_to_noncentrality(theta, params);
}

ccl_device_inline float solve_noncentrality(float3 const point, float4 const params)
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

CCL_NAMESPACE_END
