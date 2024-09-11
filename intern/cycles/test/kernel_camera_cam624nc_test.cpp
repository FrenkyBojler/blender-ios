
#include <cmath>
#include <iostream>

#include "testing/testing.h"

#include "util/math.h"
#include "util/types.h"

#include "kernel/device/cpu/compat.h"
#include "kernel/device/cpu/globals.h"

#include "kernel/types.h"

#include "kernel/camera/calibrated_camera.h"
#include "kernel/camera/camera.h"
#include "kernel/camera/projection.h"

#include <random>

CCL_NAMESPACE_BEGIN

template<class VEC> bool near_vec(VEC const a, VEC const b, double const thresh)
{
  return len(a - b) < thresh;
}

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

  double sum = 0.0;

  size_t count = 0;

  void push(float const val)
  {
    max = std::max(max, std::abs(val));
    sum += val;
    count++;
  }

  void push(float const a, float const b)
  {
    push(std::abs(a - b));
  }

  double mean() const
  {
    return sum / count;
  }
};
std::ostream &operator<<(std::ostream &out, MaxError const &val)
{
  out << val.max;
  return out;
}

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

TEST(KernelCamera, calibrated_cam_radial_trivial)
{
  float const radial[6] = {0, 0, 0, 0, 0, 0};
  size_t const num_pts = 10'000;
  float const max_angle = M_PI_F;
  for (size_t ii = 0; ii <= num_pts; ++ii) {
    float const angle = float(ii) * max_angle / num_pts;
    float const forward = radial_forward(angle, radial);
    EXPECT_NEAR(angle, forward, 1e-8);
    float const solved = solve_radial(angle, radial);
    EXPECT_NEAR(solved, forward, 1e-8);
    float const derivative = radial_forward_derivative(angle, radial);
    EXPECT_NEAR(derivative, 1.0f, 1e-8);
  }
}

TEST(KernelCamera, calibrated_cam_thinprism_trivial)
{
  std::mt19937_64 rng(0xBEEBBEEB);
  std::uniform_real_distribution<float> dist(-2.0f, 2.0f);
  float2 const p = zero_float2();
  float4 const s = zero_float4();
  size_t const num_pts = 10'000;
  for (size_t ii = 0; ii <= num_pts; ++ii) {
    float2 const pt = {dist(rng), dist(rng)};
    float2 const forward = tangential_thinprism_forward(pt, p, s);
    EXPECT_NEAR(pt.x, forward.x, 1e-8);
    EXPECT_NEAR(pt.y, forward.y, 1e-8);
    float2 const solved = solve_tangential_thinprism(pt, p, s);
    EXPECT_NEAR(pt.x, solved.x, 1e-8);
    EXPECT_NEAR(pt.y, solved.y, 1e-8);
    float4 const derivative = tangential_thinprism_forward_jacobian(pt, p, s);
    EXPECT_NEAR(derivative.x, 1.0f, 1e-8);
    EXPECT_NEAR(derivative.y, 0.0f, 1e-8);
    EXPECT_NEAR(derivative.z, 0.0f, 1e-8);
    EXPECT_NEAR(derivative.w, 1.0f, 1e-8);
  }
}

void test_tangential_thinprism_solver(float2 const tangential,
                                      float4 const thin_prism,
                                      float const fov_deg,
                                      std::string const &prefix,
                                      float const error_treshold)
{
  float const fov_rad = fov_deg * M_PI_F / 180.0;
  size_t num_samples = 1'000;
  size_t num_samples_per_axis = std::sqrt(num_samples);

  MaxError error_x;
  MaxError error_y;
  MaxError error_length;

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
      EXPECT_LT(solution_error_squared, 1e-12) << prefix;

      EXPECT_PRED3(near_vec<float2>, pt, solved, error_treshold);

      error_x.push(pt.x, solved.x);
      error_y.push(pt.y, solved.y);
      error_length.push(len(pt - solved));
    }
  }

  if (testing::Test::HasFailure()) {
    std::cout << "Maximum error in x for tangential_thinprism_forward " << prefix << ": "
              << error_x.max << std::endl;
    std::cout << "Maximum error in y for tangential_thinprism_forward " << prefix << ": "
              << error_y.max << std::endl;
    std::cout << "Maximum error in length for tangential_thinprism_forward " << prefix << ": "
              << error_length.max << std::endl;
  }
}

TEST(KernelCamera, calibrated_cam_tangential_thin_prism)
{
  float2 const p{-1.7905108189099640e-04, 3.6947302643007590e-06};
  // TODO: Replace these values by values obtained from real calibrations
  float4 const s{-2e-4, 1e-4, 3e-4, -5e-4};

  test_tangential_thinprism_solver(zero_float2(), zero_float4(), 355.0f, "all-zero", 1e-15);

  test_tangential_thinprism_solver(p, s, 165.0f, "normal", 2e-7);

  test_tangential_thinprism_solver(zero_float2(), s * 20.0f, 165.0f, "exaggerated", 2e-7);

  test_tangential_thinprism_solver(p * 20.0f, s * 20.0f, 165.0f, "very-exaggerated", 2e-7);
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
void test_radial_solver(float const *const radial,
                        float const fov_deg,
                        std::string const &prefix,
                        float const error_threshold)
{
  size_t const num_samples = 1'000;
  MaxError error;
  for (size_t ii = 0; ii <= num_samples; ++ii) {
    double const angle_rad = deg2rad(ii * 0.5f * fov_deg / num_samples);
    double const angle_tgt = radial_forward(angle_rad, radial);
    double const solution = solve_radial(angle_tgt, radial);
    EXPECT_NEAR(solution, angle_rad, error_threshold) << prefix;
    error.push(solution, angle_rad);
  }
  EXPECT_LT(error.mean(), 2e-8) << prefix;

  if (::testing::Test::HasFailure()) {
    std::cout << "Maximum and mean error for radial solver with " << prefix << ": " << error.max
              << ", " << error.mean() << std::endl;
  }
}

TEST(KernelCamera, calibrated_cam_radial)
{
  {
    // Trivial case with all distortion coefficients zero:
    float const k[]{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    // solve_radial with all distortion coefficients zero is almost a no-op,
    // therefore we use a very small error threshold.
    test_radial_solver(k, 355.0f, "perfect-equidistant", 1e-15);
  }
  {
    // Testcase from a real calib of a lens which is very non-equidistant:
    float const k[]{-6.3212689067106823e-02f,
                    1.0783254109563612e-02f,
                    -1.5666209452467651e-02f,
                    1.0487251796288639e-02f,
                    -3.8781789116892440e-03f,
                    5.6914433571826422e-04f};

    test_radial_solver(k, 165.0f, "non-equidistant", 4e-7);
  }

  {
    // Testcase from a real calib of a lens which is almost equidistant.
    float const k[]{6.8925238237090430e-03f,
                    4.9065099682158737e-03f,
                    -5.6645010933102091e-03f,
                    3.5255596948621580e-03f,
                    -1.2505361069399053e-03f,
                    1.6815868507166388e-04f};

    test_radial_solver(k, 170.0f, "almost-equidistant", 4e-7);
  }

  {
    // Testcase from a real calib of some roughly "orthographic fisheye" lens.
    float const k[]{-2.7071250929750468e-01,
                    5.1892349368743274e-01,
                    -1.0625944626790622e+00,
                    1.1393696445669612e+00,
                    -6.2508654084092763e-01,
                    1.4034706020688248e-01};

    test_radial_solver(k, 122.0f, "orthographic", 4e-7);
  }
}

TEST(KernelCamera, calibrated_cam_noncentrality)
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

TEST(KernelCamera, calibrated_cam_apply_projection_type_roundtrip)
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

TEST(KernelCamera, calibrated_cam_calibrated_cam_to_direction_simple)
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

      const float4 computed = calibrated_cam_to_direction(
          sensor, width, height, fov, focal, EQUIDISTANT, radial, zero_float2(), zero_float4());

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

      // Verify that calibrated_cam_to_expected returns all zeroes if the point is outside
      // the configured field of view.

      const float4 computed_outside_fov = calibrated_cam_to_direction(sensor,
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

      const float2 round_trip = direction_to_calibrated_cam(
          expected, width, height, focal, EQUIDISTANT, radial, zero_float2(), zero_float4());

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
 * @brief test_calibrated_cam_roundtrip tests the correctness of calibrated_cam_to_direction using
 * a given set of distortion parameters. It has assertions for the difference between the original
 * angle and the solution found by calibrated_cam_to_direction.
 *
 * @param _k
 * @param fov_deg
 * @param prefix
 */
void test_calibrated_cam_roundtrip(float const *const radial,
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
    float const cos_phi = cosf(phi);
    float const sin_phi = sinf(phi);
    float4 const expected_dir{cosf(theta), -cos_phi * sinf(theta), sin_phi * sinf(theta), theta};
    float2 const sensor = direction_to_calibrated_cam(
        expected_dir, width, height, focal, EQUIDISTANT, radial, tangential, thin_prism);
    float4 const recomputed_dir = calibrated_cam_to_direction(
        sensor, width, height, fov, focal, EQUIDISTANT, radial, tangential, thin_prism);

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

    float2 const recomputed_sensor = direction_to_calibrated_cam(
        recomputed_dir, width, height, focal, EQUIDISTANT, radial, tangential, thin_prism);
    error_sensor_x.push(sensor.x, recomputed_sensor.x);
    EXPECT_NEAR(sensor.x, recomputed_sensor.x, error_threshold)
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
    EXPECT_NEAR(sensor.y, recomputed_sensor.y, error_threshold)
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
    std::cout << "Maximum error stats for " << prefix << std::endl;
    std::cout << "Maximum error in x " << prefix << ": " << error_x.max << std::endl;
    std::cout << "Maximum error in y for " << prefix << ": " << error_y.max << std::endl;
    std::cout << "Maximum error in z for " << prefix << ": " << error_z.max << std::endl;
    std::cout << "Maximum error in theta for " << prefix << ": " << error_theta.max << std::endl;
    std::cout << "Maximum error in sensor_x for " << prefix << ": " << error_sensor_x.max
              << std::endl;
    std::cout << "Maximum error in sensor_y for " << prefix << ": " << error_sensor_y.max
              << std::endl;
  }
}

TEST(KernelCamera, calibrated_cam_calibrated_cam_to_direction_round_trip)
{
  float2 const p{2.0e-04, -2.0e-04};
  // TODO: Replace these values by values obtained from real calibrations
  float4 const s{-2e-4, 2e-4, 2e-4, -2e-4};

  float const zero_radial[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  test_calibrated_cam_roundtrip(zero_radial,
                                {2e-4, 0.0f},
                                zero_float4(),
                                180.0f,
                                "zero radial, zero thin_prism, normal tangential_x",
                                2e-7);

  test_calibrated_cam_roundtrip(zero_radial,
                                {0.0f, 2e-4f},
                                zero_float4(),
                                180.0f,
                                "zero radial, zero thin_prism, normal tangential_y",
                                3e-7);

  test_calibrated_cam_roundtrip(zero_radial,
                                p,
                                zero_float4(),
                                180.0f,
                                "zero radial, zero thin_prism, normal tangential",
                                2e-7);

  test_calibrated_cam_roundtrip(zero_radial,
                                10.0f * p,
                                zero_float4(),
                                180.0f,
                                "zero radial, zero thin_prism, exaggerated tangential",
                                2e-7);

  test_calibrated_cam_roundtrip(zero_radial,
                                zero_float2(),
                                s,
                                180.0f,
                                "zero radial, zero tangential, normal thin-prism",
                                2e-7);

  test_calibrated_cam_roundtrip(zero_radial,
                                zero_float2(),
                                10.0f * s,
                                180.0f,
                                "zero radial, zero tangential, exaggerated thin-prism",
                                3e-7);

  {
    // Testcase from a real calib of a lens which is very non-equidistant:
    float const k[6]{-6.3212689067106823e-02f,
                     1.0783254109563612e-02f,
                     -1.5666209452467651e-02f,
                     1.0487251796288639e-02f,
                     -3.8781789116892440e-03f,
                     5.6914433571826422e-04f};

    test_calibrated_cam_roundtrip(k, p, s, 165.0f, "non-equidistant", 4e-7);
  }

  {
    // Testcase from a real calib of a lens which is almost equidistant.
    float const k[6]{6.8925238237090430e-03f,
                     4.9065099682158737e-03f,
                     -5.6645010933102091e-03f,
                     3.5255596948621580e-03f,
                     -1.2505361069399053e-03f,
                     1.6815868507166388e-04f};

    test_calibrated_cam_roundtrip(k, p, s, 170.0f, "almost-equidistant", 2e-7);
  }

  {
    // Testcase from a real calib of some roughly "orthographic fisheye" lens.
    float const k[6]{-2.7071250929750468e-01,
                     5.1892349368743274e-01,
                     -1.0625944626790622e+00,
                     1.1393696445669612e+00,
                     -6.2508654084092763e-01,
                     1.4034706020688248e-01};

    test_calibrated_cam_roundtrip(k, p, s, 122.0f, "orthographic", 3e-7);
  }
}

TEST(KernelCamera, calibrated_cam_vs_reference_projections)
{
  MaxError max_error_x, max_error_y, max_error_z, max_error_theta, max_error_sensor_x,
      max_error_sensor_y, max_error_length;

  float const fov = 5;

  float const focal_ref = 0.318310f;
  float const radial[6] = {-0.271000f, 0.519000f, -1.060000f, 1.140000f, -0.625000f, 0.140000f};
  float2 const p = make_float2(-0.057100f, 0.076120f);
  float4 const s = make_float4(0.001000f, 0.002000f, -0.002000f, 0.001000f);
  std::pair<float2, float4> reference_data[]{
      {make_float2(0.544857f, 0.457416f),
       make_float4(0.981022f, -0.142974f, -0.130976f, 0.195134f)},
      {make_float2(0.491441f, 0.580892f), make_float4(0.962164f, 0.023821f, 0.271428f, 0.275961f)},
      {make_float2(0.430290f, 0.409030f),
       make_float4(0.943426f, 0.201748f, -0.263145f, 0.337981f)},
      {make_float2(0.609617f, 0.516467f),
       make_float4(0.924807f, -0.374620f, 0.066265f, 0.390268f)},
      {make_float2(0.383539f, 0.567937f), make_float4(0.906308f, 0.356586f, 0.226831f, 0.436332f)},
      {make_float2(0.536058f, 0.347188f),
       make_float4(0.887927f, -0.119414f, -0.444214f, 0.477978f)},
      {make_float2(0.561047f, 0.620046f),
       make_float4(0.869664f, -0.227524f, 0.438084f, 0.516275f)},
      {make_float2(0.326487f, 0.431959f),
       make_float4(0.851519f, 0.492509f, -0.179864f, 0.551922f)},
      {make_float2(0.652088f, 0.427871f),
       make_float4(0.833490f, -0.510732f, -0.210823f, 0.585401f)},
      {make_float2(0.420486f, 0.649567f), make_float4(0.815579f, 0.245257f, 0.524099f, 0.617067f)},
      {make_float2(0.430310f, 0.289524f),
       make_float4(0.797784f, 0.180451f, -0.575307f, 0.647185f)},
      {make_float2(0.646058f, 0.579396f),
       make_float4(0.780105f, -0.541318f, 0.313705f, 0.675963f)},
      {make_float2(0.282058f, 0.536444f), make_float4(0.762541f, 0.631851f, 0.138911f, 0.703565f)},
      {make_float2(0.618735f, 0.309560f),
       make_float4(0.745092f, -0.383589f, -0.545616f, 0.730124f)},
      {make_float2(0.516595f, 0.680370f),
       make_float4(0.727758f, -0.088137f, 0.680148f, 0.755750f)},
      {make_float2(0.298195f, 0.325499f),
       make_float4(0.710537f, 0.538053f, -0.453471f, 0.780535f)},
      {make_float2(0.701941f, 0.478984f),
       make_float4(0.693430f, -0.719908f, -0.029770f, 0.804557f)},
      {make_float2(0.327821f, 0.649062f), make_float4(0.676437f, 0.522053f, 0.519513f, 0.827882f)},
      {make_float2(0.502392f, 0.221553f),
       make_float4(0.659556f, -0.034720f, -0.750853f, 0.850568f)},
      {make_float2(0.623151f, 0.645919f),
       make_float4(0.642788f, -0.490812f, 0.588156f, 0.872665f)},
  };

  for (auto [sensor, expected] : reference_data) {
    for (float const scale : {1.0f}) {
      const float width = 1.0f / scale;
      const float height = 1.0f / scale;
      float const focal = focal_ref / scale;

      const float4 computed = calibrated_cam_to_direction(
          sensor, width, height, fov, focal, EQUIDISTANT, radial, p, s);

      float const length = len(make_float3(computed.x, computed.y, computed.z));
      EXPECT_NEAR(length, 1.0f, 1e-6);
      max_error_length.push(length, 1.0f);

      max_error_x.push(expected.x, computed.x);
      EXPECT_NEAR(expected.x, computed.x, 3e-6f)
          << "sensor: " << sensor << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << expected << std::endl
          << "difference: " << computed - expected << std::endl;
      max_error_y.push(expected.y, computed.y);
      EXPECT_NEAR(expected.y, computed.y, 3e-6f)
          << "sensor: " << sensor << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << expected << std::endl
          << "difference: " << computed - expected << std::endl;
      max_error_z.push(expected.z, computed.z);
      EXPECT_NEAR(expected.z, computed.z, 3e-6f)
          << "sensor: " << sensor << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << expected << std::endl
          << "difference: " << computed - expected << std::endl;
      max_error_theta.push(expected.w, computed.w);
      EXPECT_NEAR(expected.w, computed.w, 5e-6f)
          << "sensor: " << sensor << std::endl
          << "scale: " << scale << std::endl
          << "computed: " << computed << std::endl
          << "expected: " << expected << std::endl
          << "difference: " << computed - expected << std::endl;

      // Check round-trip consistency

      const float2 round_trip = direction_to_calibrated_cam(
          computed, width, height, focal, EQUIDISTANT, radial, p, s);

      max_error_sensor_x.push(round_trip.x, sensor.x);
      EXPECT_NEAR(round_trip.x, sensor.x, 1e-6f) << "sensor: " << sensor << std::endl
                                                 << "scale: " << scale << std::endl
                                                 << "round_trip: " << round_trip << std::endl;

      max_error_sensor_y.push(round_trip.y, sensor.y);
      EXPECT_NEAR(round_trip.y, sensor.y, 1e-6f) << "sensor: " << sensor << std::endl
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

    std::cout << "Maximum error of direction vector length: " << max_error_length << std::endl;
  }
}

CCL_NAMESPACE_END
