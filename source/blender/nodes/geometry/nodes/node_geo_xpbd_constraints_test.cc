/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_rotation.hh"

#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_pointcloud.hh"

#include "NOD_xpbd_constraints.hh"
#include "NOD_xpbd_solver.hh"

#include "CLG_log.h"

#include "testing/testing.h"

#include <iostream>

namespace blender::nodes::tests {

#define EXPECT_EIGEN_MATRIX_NEAR(a, b, eps) \
  do { \
    for (const int col : IndexRange((b).cols())) { \
      for (const int row : IndexRange((b).rows())) { \
        EXPECT_NEAR((a)[col][row], (b).coeff(row, col), eps); \
      } \
    } \
  } while (false);

#define EXPECT_EIGEN_V3_DIAG_NEAR(a, b, eps) \
  do { \
    EXPECT_NEAR((a)[0], (b).coeff(0, 0), eps); \
    EXPECT_NEAR((a)[1], (b).coeff(1, 1), eps); \
    EXPECT_NEAR((a)[2], (b).coeff(2, 2), eps); \
  } while (false);

#define EXPECT_EIGEN_V4_DIAG_NEAR(a, b, eps) \
  do { \
    EXPECT_NEAR((a)[0], (b).coeff(0, 0), eps); \
    EXPECT_NEAR((a)[1], (b).coeff(1, 1), eps); \
    EXPECT_NEAR((a)[2], (b).coeff(2, 2), eps); \
    EXPECT_NEAR((a)[3], (b).coeff(3, 3), eps); \
  } while (false);

#define EXPECT_EIGEN_V3_COL_NEAR(a, b, eps) \
  do { \
    EXPECT_NEAR((a)[0], (b).coeff(0, 0), eps); \
    EXPECT_NEAR((a)[1], (b).coeff(1, 0), eps); \
    EXPECT_NEAR((a)[2], (b).coeff(2, 0), eps); \
  } while (false);

#define EXPECT_EIGEN_V4_COL_NEAR(a, b, eps) \
  do { \
    EXPECT_NEAR((a)[0], (b).coeff(0, 0), eps); \
    EXPECT_NEAR((a)[1], (b).coeff(1, 0), eps); \
    EXPECT_NEAR((a)[2], (b).coeff(2, 0), eps); \
    EXPECT_NEAR((a)[3], (b).coeff(3, 0), eps); \
  } while (false);

#define EXPECT_EIGEN_V3_ROW_NEAR(a, b, eps) \
  do { \
    EXPECT_NEAR((a)[0], (b).coeff(0, 0), eps); \
    EXPECT_NEAR((a)[1], (b).coeff(0, 1), eps); \
    EXPECT_NEAR((a)[2], (b).coeff(0, 2), eps); \
  } while (false);

#define EXPECT_EIGEN_V4_ROW_NEAR(a, b, eps) \
  do { \
    EXPECT_NEAR((a)[0], (b).coeff(0, 0), eps); \
    EXPECT_NEAR((a)[1], (b).coeff(0, 1), eps); \
    EXPECT_NEAR((a)[2], (b).coeff(0, 2), eps); \
    EXPECT_NEAR((a)[3], (b).coeff(0, 3), eps); \
  } while (false);

class XPBDSolverTest : public testing::Test {
 public:
  static void SetUpTestSuite()
  {
    CLG_init();
    BKE_idtype_init();
  }

  static void TearDownTestSuite()
  {
    CLG_exit();
  }

  void SetUp() override {}

  void TearDown() override {}
};

/* Helper struct for checking that each variable is only written by one constraint.*/
template<bool enable> struct VariableChecker;

template<> struct VariableChecker<false> {
  VariableChecker(const IndexRange /*range*/) {}

  bool claim_variable(const int /*index*/)
  {
    return true;
  }

  bool has_overlap() const
  {
    return false;
  }
};

template<> struct VariableChecker<true> {
  Array<std::atomic_bool> variable_written;
  std::atomic_bool variable_overlap = false;

  VariableChecker(const IndexRange range)
  {
    variable_written.reinitialize(range.size());
    for (const int i : variable_written.index_range()) {
      variable_written[i].store(false, std::memory_order::memory_order_relaxed);
    }
  }

  bool claim_variable(const int index)
  {
    if (variable_written[index].exchange(true, std::memory_order_relaxed)) {
      variable_overlap.store(true, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  bool has_overlap() const
  {
    return variable_overlap.load(std::memory_order_relaxed);
  }
};

TEST(xpbd_constraints, PositionGoal)
{
  float lambda = 0.0f;
  float3 position = {1, 2, 3};
  const float3 goal = {-2, 0, 2};
  const float delta = math::length(float3(-3, -2, -1));
  EXPECT_NEAR(delta, 3.741657f, 1e-5f);

  const float alpha = 0.0f;

  xpbd_constraints::apply_position_goal(goal, alpha, lambda, position);
  EXPECT_NEAR(-delta, lambda, 1e-5f);
  EXPECT_V3_NEAR(goal, position, 1e-5f);
}

TEST(xpbd_constraints, RotationGoal)
{
  float lambda = 0.0f;
  math::Quaternion rotation = math::to_quaternion(
      math::AxisAngle(math::normalize(float3(-1, 2, -3)), math::AngleRadian::from_degree(45)));
  const math::Quaternion goal = math::to_quaternion(
      math::AxisAngle(math::normalize(float3(4, 4, 2)), math::AngleRadian::from_degree(-30)));
  const math::AxisAngle delta = math::to_axis_angle(math::invert(goal) * rotation);
  EXPECT_NEAR(delta.angle().radian(), 0.896427f, 1e-5f);
  EXPECT_V3_NEAR(delta.axis(), float3(-0.023005f, 0.925597f, -0.37781f), 1e-5f);

  const float alpha = 0.0f;

  xpbd_constraints::apply_rotation_goal<false>(goal, alpha, lambda, rotation);
  EXPECT_NEAR(-delta.angle().radian(), lambda, 1e-5f);
  EXPECT_V4_NEAR(float4(goal), float4(rotation), 1e-5f);
}

TEST(xpbd_constraints, RotationGoalLinearized)
{
  float lambda = 0.0f;
  math::Quaternion rotation = math::to_quaternion(
      math::AxisAngle(math::normalize(float3(-1, 2, -3)), math::AngleRadian::from_degree(45)));
  const math::Quaternion goal = math::to_quaternion(
      math::AxisAngle(math::normalize(float3(4, 4, 2)), math::AngleRadian::from_degree(-30)));
  const math::AxisAngle delta = math::to_axis_angle(math::invert(goal) * rotation);
  EXPECT_NEAR(delta.angle().radian(), 0.896427f, 1e-5f);
  EXPECT_V3_NEAR(delta.axis(), float3(-0.023005f, 0.925597f, -0.37781f), 1e-5f);

  const float alpha = 0.0f;

  xpbd_constraints::apply_rotation_goal<true>(goal, alpha, lambda, rotation);
  EXPECT_NEAR(-delta.angle().radian(), lambda, 1e-5f);
  /* Linearized quaternion offset does not yield exact rotation for large offsets. */
  EXPECT_V4_NEAR(float4(goal), float4(rotation), 0.05f);
}

TEST(xpbd_constraints, InactiveContact)
{
  float lambda = 0.0f;
  float3 position1 = {0, 0, 0};
  float3 position2 = {0, 0, 0};
  math::Quaternion rotation1 = math::Quaternion::identity();
  math::Quaternion rotation2 = math::Quaternion::identity();

  /* One-sided collision. */
  const float weight_pos1 = 1.0f;
  const float weight_pos2 = 0.0f;
  const float weight_rot1 = 1.0f;
  const float weight_rot2 = 0.0f;

  const float3 local_position1 = {0, 0, 1};
  const float3 local_position2 = {0, 0, -1};
  const float3 normal = {0, 0, 1};

  const float alpha = 0.0f;

  bool active = xpbd_constraints::apply_position_contact(weight_pos1,
                                                         weight_pos2,
                                                         weight_rot1,
                                                         weight_rot2,
                                                         local_position1,
                                                         local_position2,
                                                         normal,
                                                         alpha,
                                                         lambda,
                                                         position1,
                                                         position2,
                                                         rotation1,
                                                         rotation2);
  EXPECT_FALSE(active);
}

TEST(xpbd_constraints, CentralContact)
{
  float lambda = 0.0f;
  float3 position1 = {0, 0, 0};
  float3 position2 = {0, 0, 0};
  math::Quaternion rotation1 = math::Quaternion::identity();
  math::Quaternion rotation2 = math::Quaternion::identity();

  /* One-sided collision. */
  const float weight_pos1 = 1.0f;
  const float weight_pos2 = 0.0f;
  const float weight_rot1 = 1.0f;
  const float weight_rot2 = 0.0f;

  const float3 local_position1 = {0, 0, -1};
  const float3 local_position2 = {0, 0, 1};
  const float3 normal = {0, 0, 1};

  const float alpha = 0.0f;

  bool active = xpbd_constraints::apply_position_contact(weight_pos1,
                                                         weight_pos2,
                                                         weight_rot1,
                                                         weight_rot2,
                                                         local_position1,
                                                         local_position2,
                                                         normal,
                                                         alpha,
                                                         lambda,
                                                         position1,
                                                         position2,
                                                         rotation1,
                                                         rotation2);

  EXPECT_TRUE(active);
  EXPECT_NEAR(2, lambda, 1e-5f);
  EXPECT_V3_NEAR(float3(0, 0, 2), position1, 1e-5f);
  EXPECT_V4_NEAR(float4(1, 0, 0, 0), float4(rotation1), 1e-5f);
}

TEST(xpbd_constraints, OffcenterContact)
{
  float lambda = 0.0f;
  float3 position1 = {0, 0, 0};
  float3 position2 = {0, 0, 0};
  math::Quaternion rotation1 = math::Quaternion::identity();
  math::Quaternion rotation2 = math::Quaternion::identity();

  /* One-sided collision. */
  const float weight_pos1 = 1.0f;
  const float weight_pos2 = 0.0f;
  const float weight_rot1 = 1.0f;
  const float weight_rot2 = 0.0f;

  const float3 local_position1 = {-1, 0, 0};
  const float3 local_position2 = {0, 0, 1};
  const float3 normal = {0, 0, 1};

  const float alpha = 0.0f;

  bool active = xpbd_constraints::apply_position_contact(weight_pos1,
                                                         weight_pos2,
                                                         weight_rot1,
                                                         weight_rot2,
                                                         local_position1,
                                                         local_position2,
                                                         normal,
                                                         alpha,
                                                         lambda,
                                                         position1,
                                                         position2,
                                                         rotation1,
                                                         rotation2);

  EXPECT_TRUE(active);
  EXPECT_NEAR(0.5f, lambda, 1e-5f);
  EXPECT_V3_NEAR(float3(0, 0, 0.5f), position1, 1e-5f);
  EXPECT_V4_NEAR(math::normalize(float4(1, 0, 0.5f, 0)), float4(rotation1), 1e-5f);
}

TEST(xpbd_constraints, ContactFriction)
{
  float lambda_restitution = 0.0f;
  float lambda_friction = 0.0f;
  float3 velocity1 = {1, 0, 0};
  float3 velocity2 = {0, 0, 0};
  float3 angular_velocity1 = {0, 0, 0};
  float3 angular_velocity2 = {0, 0, 0};
  const float3 orig_velocity1 = {0, 0, 0};
  const float3 orig_velocity2 = {0, 0, 0};
  const float3 orig_angular_velocity1 = {0, 0, 0};
  const float3 orig_angular_velocity2 = {0, 0, 0};

  const float3 local_position1 = {1, 1, 0};
  const float3 local_position2 = {0, 0, 0};
  const float3 normal = {0, 0, 1};

  const float restitution = 0.0f;
  const float friction = 0.5f;
  const float threshold_normal_velocity = 0.0f;

  xpbd_constraints::apply_velocity_contact(orig_velocity1,
                                           orig_velocity2,
                                           orig_angular_velocity1,
                                           orig_angular_velocity2,
                                           local_position1,
                                           local_position2,
                                           normal,
                                           restitution,
                                           friction,
                                           threshold_normal_velocity,
                                           lambda_restitution,
                                           lambda_friction,
                                           velocity1,
                                           velocity2,
                                           angular_velocity1,
                                           angular_velocity2);
  EXPECT_EQ(0, lambda_restitution);
  EXPECT_NEAR(-0.5f, lambda_friction, 1e-5f);
  EXPECT_V3_NEAR(float3(0.5f, 0, 0), velocity1, 1e-5f);
  /* Friction impulse (-0.5, 0, 0) at point (1, 1, 0) causes torque around the Z axis. */
  EXPECT_V3_NEAR(float3(0, 0, 0.5f), angular_velocity1, 1e-5f);
}

TEST(xpbd_constraints, ContactRestitution)
{
  float lambda_restitution = 0.0f;
  float lambda_friction = 0.0f;
  float3 velocity1 = {1, 0, -1};
  float3 velocity2 = {0, 0, 0};
  float3 angular_velocity1 = {0, 0, 0};
  float3 angular_velocity2 = {0, 0, 0};
  const float3 orig_velocity1 = velocity1;
  const float3 orig_velocity2 = velocity2;
  const float3 orig_angular_velocity1 = angular_velocity1;
  const float3 orig_angular_velocity2 = angular_velocity2;

  const float3 local_position1 = {1, 1, 0};
  const float3 local_position2 = {0, 0, 0};
  const float3 normal = {0, 0, 1};

  const float restitution = 0.5f;
  const float friction = 0.0f;
  const float threshold_normal_velocity = 0.0f;

  xpbd_constraints::apply_velocity_contact(orig_velocity1,
                                           orig_velocity2,
                                           orig_angular_velocity1,
                                           orig_angular_velocity2,
                                           local_position1,
                                           local_position2,
                                           normal,
                                           restitution,
                                           friction,
                                           threshold_normal_velocity,
                                           lambda_restitution,
                                           lambda_friction,
                                           velocity1,
                                           velocity2,
                                           angular_velocity1,
                                           angular_velocity2);
  EXPECT_EQ(0.5f, lambda_restitution);
  EXPECT_NEAR(0, lambda_friction, 1e-5f);
  EXPECT_V3_NEAR(float3(1.0f, 0, 0.5f), velocity1, 1e-5f);
  /* Rebound impulse (0, 0, 0.5) at point (1, 1, 0) causes torque around X and Y. */
  EXPECT_V3_NEAR(float3(1.5f, -1.5f, 0), angular_velocity1, 1e-5f);
}

TEST(xpbd_constraints, StretchShear)
{
  const float edge_length = 2.0f;

  const float alpha = 0.0f;

  /* Position 1 only. */
  {
    float3 lambda = float3(0.0f);
    float3 position1 = {-1, 0, 0};
    float3 position2 = {0, 0, 1};
    math::Quaternion rotation = math::Quaternion::identity();
    xpbd_constraints::apply_position_stretch_shear<false>(
        1, 0, 0, edge_length, alpha, lambda, position1, position2, rotation);
    EXPECT_V3_NEAR(float3(2, 0, -2), lambda, 1e-5f);
    EXPECT_V3_NEAR(float3(0, 0, -1), position1, 1e-5f);
    EXPECT_V3_NEAR(float3(0, 0, 1), position2, 1e-5f);
    /* XXX BROKEN does not take rotation weight into account. */
    // EXPECT_V4_NEAR(float4(1, 0, 0, 0), float4(rotation), 1e-5f);
  }

  /* Position 2 only. */
  {
    float3 lambda = float3(0.0f);
    float3 position1 = {-1, 0, 0};
    float3 position2 = {0, 0, 1};
    math::Quaternion rotation = math::Quaternion::identity();
    xpbd_constraints::apply_position_stretch_shear<false>(
        0, 1, 0, edge_length, alpha, lambda, position1, position2, rotation);
    EXPECT_V3_NEAR(float3(2, 0, -2), lambda, 1e-5f);
    EXPECT_V3_NEAR(float3(-1, 0, 0), position1, 1e-5f);
    EXPECT_V3_NEAR(float3(-1, 0, 2), position2, 1e-5f);
    /* XXX BROKEN does not take rotation weight into account. */
    // EXPECT_V4_NEAR(float4(1, 0, 0, 0), float4(rotation), 1e-5f);
  }

  /* Rotation only. */
  {
    float3 lambda = float3(0.0f);
    float3 position1 = {-1, 0, 0};
    float3 position2 = {0, 0, 1};
    math::Quaternion rotation = math::Quaternion::identity();
    xpbd_constraints::apply_position_stretch_shear<false>(
        0, 0, 1, edge_length, alpha, lambda, position1, position2, rotation);
    EXPECT_V3_NEAR(float3(0.125f, 0, -0.125f), lambda, 1e-5f);
    EXPECT_V3_NEAR(float3(-1, 0, 0), position1, 1e-5f);
    EXPECT_V3_NEAR(float3(0, 0, 1), position2, 1e-5f);
    const math::AxisAngle axis_angle = math::to_axis_angle(rotation);
    EXPECT_V3_NEAR(axis_angle.axis(), float3(0, 1, 0), 1e-5f);
    EXPECT_NEAR(axis_angle.angle().degree(), 45.0f, 1e-5f);
  }
  /* Rotation only with linearized quaternions. */
  {
    float3 lambda = float3(0.0f);
    float3 position1 = {-1, 0, 0};
    float3 position2 = {0, 0, 1};
    math::Quaternion rotation = math::Quaternion::identity();
    xpbd_constraints::apply_position_stretch_shear<true>(
        0, 0, 1, edge_length, alpha, lambda, position1, position2, rotation);
    EXPECT_V3_NEAR(float3(0.125f, 0, -0.125f), lambda, 1e-5f);
    EXPECT_V3_NEAR(float3(-1, 0, 0), position1, 1e-5f);
    EXPECT_V3_NEAR(float3(0, 0, 1), position2, 1e-5f);
    const math::AxisAngle axis_angle = math::to_axis_angle(rotation);
    EXPECT_V3_NEAR(axis_angle.axis(), float3(0, 1, 0), 1e-5f);
    // XXX BROKEN
    // EXPECT_NEAR(axis_angle.angle().degree(), 45.0f, 0.01f);
  }
}

TEST(xpbd_constraints, BendTwist)
{
  constexpr float x = 0.707107f;

  const math::Quaternion target_rotation1 = math::Quaternion::identity();
  const math::Quaternion target_rotation2 = math::to_quaternion(
      math::AxisAngle(math::AxisSigned::X_POS, math::AngleRadian::from_degree(90.0f)));
  EXPECT_V4_NEAR(float4(1, 0, 0, 0), float4(target_rotation1), 1e-5f);
  EXPECT_V4_NEAR(float4(x, x, 0, 0), float4(target_rotation2), 1e-5f);
  const float3 darboux_vector =
      (math::invert(target_rotation1) * target_rotation2).imaginary_part();
  EXPECT_V3_NEAR(float3(x, 0, 0), darboux_vector, 1.e-5f);

  const float alpha = 0.0f;

  /* Rotation 1 only. */
  {
    float3 lambda = float3(0.0f);
    math::Quaternion rotation1 = math::Quaternion::identity();
    math::Quaternion rotation2 = math::Quaternion::identity();
    xpbd_constraints::apply_position_bend_twist<true>(
        1, 0, darboux_vector, alpha, lambda, rotation1, rotation2);
    EXPECT_V3_NEAR(float3(x, 0, 0), lambda, 1e-5f);
    EXPECT_V4_NEAR(math::normalize(float4(1.0f, -x, 0, 0)), float4(rotation1), 1e-5f);
    EXPECT_V4_NEAR(float4(1, 0, 0, 0), float4(rotation2), 1e-5f);
  }
  /* Rotation 2 only. */
  {
    float3 lambda = float3(0.0f);
    math::Quaternion rotation1 = math::Quaternion::identity();
    math::Quaternion rotation2 = math::Quaternion::identity();
    xpbd_constraints::apply_position_bend_twist<true>(
        0, 1, darboux_vector, alpha, lambda, rotation1, rotation2);
    EXPECT_V3_NEAR(float3(x, 0, 0), lambda, 1e-5f);
    EXPECT_V4_NEAR(float4(1, 0, 0, 0), float4(rotation1), 1e-5f);
    EXPECT_V4_NEAR(math::normalize(float4(1.0f, x, 0, 0)), float4(rotation2), 1e-5f);
  }
}

TEST(xpbd_constraints, VariableOverlapCheckerPass)
{
  const IndexRange range(10);

  VariableChecker<true> var_checker(range);
  VariableChecker<false> var_checker_disabled(range);
  EXPECT_FALSE(var_checker.has_overlap());

  Array<int> vars = {2, 7, 9, 0, 3, 1, 4};
  for (const int i : vars) {
    var_checker.claim_variable(i);
    var_checker_disabled.claim_variable(i);
  }
  EXPECT_FALSE(var_checker.has_overlap());
  EXPECT_FALSE(var_checker_disabled.has_overlap());
}

TEST(xpbd_constraints, VariableOverlapCheckerFail)
{
  const IndexRange range(10);

  VariableChecker<true> var_checker(range);
  VariableChecker<false> var_checker_disabled(range);
  EXPECT_FALSE(var_checker.has_overlap());

  Array<int> vars = {2, 7, 3, 5, 9, 0, 0, 3, 1, 7, 4};
  for (const int i : vars) {
    var_checker.claim_variable(i);
    var_checker_disabled.claim_variable(i);
  }
  EXPECT_TRUE(var_checker.has_overlap());
  EXPECT_FALSE(var_checker_disabled.has_overlap());
}

struct SolverTestData {
  xpbd_constraints::ConstraintEvalParams params;
  xpbd_constraints::ConstraintVariables vars;
  Vector<xpbd_constraints::ConstraintEvalData> data;

  /* These are the same arrays stored in point cloud attributes,
   * put here directly for convenient testing. */
  struct {
    Array<int> point1;
    Array<float> lambdas;
    Array<float> alphas;
    Array<float> betas;
    Array<float3> goal_position;
  } position_goal;
  struct {
    Array<int> point1;
    Array<int> point2;
    Array<float3> lambdas;
    Array<float3> alphas;
    Array<float3> betas;
    Array<float3> darboux_vector;
  } bend_twist;
  struct {
    Array<int> point1;
    Array<int> collider_index;
    Array<float> lambdas;
    Array<float> alphas;
    Array<float> betas;
    Array<float3> local_position1;
    Array<float3> local_position2;
    Array<float3> normal;
  } contact;
};

static SolverTestData simple_solver_data(const bool use_velocities)
{
  SolverTestData solver_test;

  solver_test.params.delta_time = 0.2f;
  solver_test.params.delta_time_squared = 0.2f * 0.2f;
  solver_test.params.inv_delta_time = 1.0f / 0.2f;
  solver_test.params.inv_delta_time_squared = 1.0f / (0.2f * 0.2f);

  solver_test.params.masses = VArray<float>::ForContainer(Array<float>{1.0f, 3.0f, 0.5f});
  solver_test.params.local_inertia = VArray<float3>::ForContainer(
      Array<float3>{float3(1.0f), float3(1, 2, 1), float3(0.2f, 10.f, 0.5f)});

  solver_test.params.old_positions = VArray<float3>::ForContainer(
      Array<float3>{float3(-1, 0, 2), float3(1, 1, 1), float3(0, 0, -2)});
  solver_test.params.old_rotations = VArray<math::Quaternion>::ForContainer(
      Array<math::Quaternion>{math::to_quaternion(math::EulerXYZ(0, -10, 0)),
                              math::to_quaternion(math::EulerXYZ(90, 0, 0)),
                              math::to_quaternion(math::EulerXYZ(100, 0, -80))});

  solver_test.vars.positions = {float3(0, 1, 0), float3(1, 0, 0), float3(0, 0, -2)};
  solver_test.vars.rotations = {math::to_quaternion(math::EulerXYZ(0, 0, 0)),
                                math::to_quaternion(math::EulerXYZ(90, 0, 0)),
                                math::to_quaternion(math::EulerXYZ(0, -20, 0))};
  if (use_velocities) {
    solver_test.vars.velocities = {float3(-1, -1, 0), float3(0, 0, 0), float3(0, 0, 4)};
    solver_test.vars.angular_velocities = {float3(0, 0, 0), float3(3, 0, -1), float3(2, 2, 0)};
  }

  /* Note not all values in 1d/3d ranges are used, depending on component width of the constraint
   * type. */
  solver_test.position_goal.point1 = {0, 2};
  solver_test.position_goal.lambdas = {0.2f, 3.0f};
  solver_test.position_goal.alphas = {1.5f, 0.1f};
  solver_test.position_goal.betas = {0.3f, 0.0f};
  solver_test.position_goal.goal_position = {float3(0.0f), float3(1, -1, 2)};

  solver_test.bend_twist.point1 = {1, 0};
  solver_test.bend_twist.point2 = {2, 1};
  solver_test.bend_twist.lambdas = {float3(-1.0f, 0.0f, 0.0f), float3(4.0f, -4.0f, 1.0f)};
  solver_test.bend_twist.alphas = {float3(0.6f, 1.1f, 0.0f), float3(0.0f, 0.0f, 3.0f)};
  solver_test.bend_twist.betas = {float3(0.5f, 0.001f, 0.5f), float3(1.0f, 1.0f, 1.0f)};
  solver_test.bend_twist.darboux_vector = {float3(0.2f, 0.8f, 1.1f), float3(-0.5f, -0.5f, 2.2f)};

  solver_test.contact.point1 = {1, 1, 0};
  solver_test.contact.collider_index = {1, 0, 1};
  solver_test.contact.lambdas = {0.0f, 0.2f, 1.0f};
  /* Compliance and damping are ignored by contact constraints. */
  solver_test.contact.alphas = {0, 0, 0};
  solver_test.contact.betas = {0, 0, 0};
  solver_test.contact.local_position1 = {
      float3(0, 0, 0), float3(0.5f, 1.0f, 0.0f), float3(-2.0f, 0.0f, 0.1f)};
  solver_test.contact.local_position2 = {
      float3(1.0f, -0.5f, 0.0f), float3(0.3f, 0.4f, -1.0f), float3(-2.0f, 0.0f, 0.1f)};
  solver_test.contact.normal = {
      float3(1.0f, 0.0f, 0.0f), float3(0.3f, 0.4f, -1.0f), float3(0.0f, 0.0f, -1.0f)};

  using AttributeInfo = std::pair<StringRef, GSpan>;
  auto add_constraint_data = [&](const xpbd_constraints::ConstraintTypeInfo &type,
                                 const Span<AttributeInfo> attribute_info) {
    PointCloud *constraints = BKE_pointcloud_new_nomain(attribute_info.first().second.size());
    bke::MutableAttributeAccessor attributes = constraints->attributes_for_write();

    for (const AttributeInfo &info : attribute_info) {
      attributes.add(info.first,
                     bke::AttrDomain::Point,
                     bke::cpp_type_to_custom_data_type(info.second.type()),
                     bke::AttributeInitVArray(GVArray::ForSpan(info.second)));
    }

    solver_test.data.append({});
    solver_test.data.last().type = &type;
    solver_test.data.last().geometry = bke::GeometrySet::from_pointcloud(std::move(constraints));
    solver_test.data.last().constraints = IndexRange(
        attributes.domain_size(bke::AttrDomain::Point));
  };
  add_constraint_data(
      xpbd_constraints::get_info__position_goal(true),
      {AttributeInfo{"point1", solver_test.position_goal.point1.as_span()},
       AttributeInfo{"lambda", solver_test.position_goal.lambdas.as_span()},
       AttributeInfo{"goal_position", solver_test.position_goal.goal_position.as_span()},
       AttributeInfo{"compliance", solver_test.position_goal.alphas.as_span()},
       AttributeInfo{"damping", solver_test.position_goal.betas.as_span()}});
  add_constraint_data(
      xpbd_constraints::get_info__bend_twist(true),
      {AttributeInfo{"point1", solver_test.bend_twist.point1.as_span()},
       AttributeInfo{"point2", solver_test.bend_twist.point2.as_span()},
       AttributeInfo{"lambda", solver_test.bend_twist.lambdas.as_span()},
       AttributeInfo{"darboux_vector", solver_test.bend_twist.darboux_vector.as_span()},
       AttributeInfo{"compliance", solver_test.bend_twist.alphas.as_span()},
       AttributeInfo{"damping", solver_test.bend_twist.betas.as_span()}});
  add_constraint_data(
      xpbd_constraints::get_info__contact(true),
      {AttributeInfo{"point1", solver_test.contact.point1.as_span()},
       AttributeInfo{"collider_index", solver_test.contact.collider_index.as_span()},
       AttributeInfo{"lambda", solver_test.contact.lambdas.as_span()},
       AttributeInfo{"compliance", solver_test.contact.alphas.as_span()},
       AttributeInfo{"damping", solver_test.contact.betas.as_span()},
       AttributeInfo{"local_position1", solver_test.contact.local_position1.as_span()},
       AttributeInfo{"local_position2", solver_test.contact.local_position2.as_span()},
       AttributeInfo{"normal", solver_test.contact.normal.as_span()}});

  return solver_test;
}

inline float4x4 quaternion_matrix(const math::Quaternion &q)
{
  float4x4 result;
  result[0] = float4{q.w, q.x, q.y, q.z};
  result[1] = float4{-q.x, q.w, -q.z, q.y};
  result[2] = float4{-q.y, q.z, q.w, -q.x};
  result[3] = float4{-q.z, -q.y, q.x, q.w};
  return result;
}

TEST_F(XPBDSolverTest, GlobalSolverConstruct)
{
  constexpr float eps = 1e-6f;

  SolverTestData solver_test = simple_solver_data(false);
  const float inv_dt = solver_test.params.inv_delta_time;
  const float inv_dt_sq = solver_test.params.inv_delta_time_squared;

  IndexMaskMemory memory;
  xpbd_constraints::GlobalSolverSystem system = xpbd_constraints::build_global_solve_system(
      solver_test.params, solver_test.data, solver_test.vars, true, memory);
  const Eigen::SparseMatrix<float> &H = system.matrix;
  const Eigen::VectorXf &b = system.target;
  /* Print matrix for debugging purposes if necessary. */
  if (false) {
    const Eigen::IOFormat format;
    std::cout << H.toDense().format(format) << std::endl;
  }
  EXPECT_EQ(29, H.rows());
  EXPECT_EQ(29, H.cols());
  EXPECT_EQ(173, H.nonZeros());
  EXPECT_EQ(29, b.rows());

  EXPECT_EIGEN_V3_DIAG_NEAR(float3(solver_test.params.masses[0]), H.block(0, 0, 3, 3), eps);
  EXPECT_EIGEN_V3_DIAG_NEAR(float3(solver_test.params.masses[1]), H.block(3, 3, 3, 3), eps);
  EXPECT_EIGEN_V3_DIAG_NEAR(float3(solver_test.params.masses[2]), H.block(6, 6, 3, 3), eps);

  const float4x4 inertia_tensor0 = quaternion_matrix(
      solver_test.vars.rotations[0] * math::Quaternion(0.0f, solver_test.params.local_inertia[0]));
  const float4x4 inertia_tensor1 = quaternion_matrix(
      solver_test.vars.rotations[1] * math::Quaternion(0.0f, solver_test.params.local_inertia[1]));
  const float4x4 inertia_tensor2 = quaternion_matrix(
      solver_test.vars.rotations[2] * math::Quaternion(0.0f, solver_test.params.local_inertia[2]));
  EXPECT_EIGEN_MATRIX_NEAR(inertia_tensor0, H.block(9, 9, 4, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(inertia_tensor1, H.block(13, 13, 4, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(inertia_tensor2, H.block(17, 17, 4, 4), eps);

  /* Residual for motion equations should be zero. */
  for (const int i : IndexRange(21)) {
    EXPECT_EQ(0.0f, b[i]);
  }

  /* Expected values for compliance entries. */
  auto compliance_f = [&](const float alpha, const float beta) {
    return alpha * inv_dt_sq / (1.0f + alpha * beta);
  };
  auto compliance_v = [&](const float3 &alpha, const float3 &beta) {
    return alpha * inv_dt_sq / (float3(1.0f) + alpha * beta);
  };

  auto target_residual_f =
      [&](const float residual, const float alpha, const float beta, const float lambda) -> float {
    return (residual + alpha * lambda * inv_dt_sq) / (1.0f + alpha * beta);
  };
  auto target_residual_v = [&](const float3 &residual,
                               const float3 &alpha,
                               const float3 &beta,
                               const float3 &lambda) -> float3 {
    return (residual + alpha * lambda * inv_dt_sq) / (float3(1.0f) + alpha * beta);
  };

  auto target_velocity_f = [&](const float alpha,
                               const float beta,
                               const int point_index,
                               const float3 &gradient) -> float {
    const float3 velocity = (solver_test.vars.positions[point_index] -
                             solver_test.params.old_positions[point_index]) *
                            inv_dt;
    return (alpha * beta * math::dot(gradient, velocity)) / (1.0f + alpha * beta);
  };
  // auto target_velocity_v = [&](const float3 &alpha,
  //                              const float3 &beta,
  //                              const int point_index,
  //                              const float4x4 &gradient) -> float3 {
  //   const float3 velocity = (solver_test.vars.positions[point_index] -
  //                            solver_test.params.old_positions[point_index]) *
  //                           inv_dt;
  //   return (alpha * beta * (velocity * gradient.view<3, 3>())) / (1.0f + alpha * beta);
  // };

  // auto target_angular_velocity_f = [&](const float alpha,
  //                                      const float beta,
  //                                      const int point_index,
  //                                      const float4x4 &gradient) -> float {
  //   const float4 velocity = (float4(solver_test.vars.rotations[point_index]) -
  //                            float4(solver_test.params.old_rotations[point_index])) *
  //                           inv_dt;
  //   /* Note: gradient is actually transpose of the Jacobian, each column is the derivative of
  //    * one constraint variable. */
  //   return (alpha * beta * math::dot(gradient[0], velocity)) / (1.0f + alpha * beta);
  // };
  auto target_angular_velocity_v = [&](const float3 &alpha,
                                       const float3 &beta,
                                       const int point_index,
                                       const float4x4 &gradient) -> float3 {
    const float4 velocity = (float4(solver_test.vars.rotations[point_index]) -
                             float4(solver_test.params.old_rotations[point_index])) *
                            inv_dt;
    /* Note: gradient is actually transpose of the Jacobian, each column is the derivative of
     * one constraint variable. */
    return (alpha * beta * (velocity * gradient.view<3, 4>())) / (float3(1.0f) + alpha * beta);
  };

  {
    const auto &test_data = solver_test.position_goal;

    EXPECT_NEAR(compliance_f(test_data.alphas[0], test_data.betas[0]), H.coeff(21, 21), eps);
    EXPECT_NEAR(compliance_f(test_data.alphas[1], test_data.betas[1]), H.coeff(22, 22), eps);

    float residual[2];
    float3 pos_gradient[2];
    xpbd_constraints::eval_position_goal_elements(test_data.goal_position[0],
                                                  solver_test.vars.positions[test_data.point1[0]],
                                                  residual[0],
                                                  pos_gradient[0]);
    xpbd_constraints::eval_position_goal_elements(test_data.goal_position[1],
                                                  solver_test.vars.positions[test_data.point1[1]],
                                                  residual[1],
                                                  pos_gradient[1]);
    EXPECT_EIGEN_V3_ROW_NEAR(pos_gradient[0], H.block(21, 0, 1, 3), eps);
    EXPECT_EIGEN_V3_COL_NEAR(pos_gradient[0], H.block(0, 21, 3, 1), eps);
    EXPECT_EIGEN_V3_ROW_NEAR(pos_gradient[1], H.block(22, 6, 1, 3), eps);
    EXPECT_EIGEN_V3_COL_NEAR(pos_gradient[1], H.block(6, 22, 3, 1), eps);

    /* Constraint lambda residuals. */
    const float target0 = target_residual_f(residual[0],
                                            test_data.alphas[0],
                                            test_data.betas[0],
                                            test_data.lambdas[0]) +
                          target_velocity_f(test_data.alphas[0],
                                            test_data.betas[0],
                                            test_data.point1[0],
                                            pos_gradient[0]);
    const float target1 = target_residual_f(residual[1],
                                            test_data.alphas[1],
                                            test_data.betas[1],
                                            test_data.lambdas[1]) +
                          target_velocity_f(test_data.alphas[1],
                                            test_data.betas[1],
                                            test_data.point1[1],
                                            pos_gradient[1]);
    EXPECT_NEAR(target0, b[21], eps);
    EXPECT_NEAR(target1, b[22], eps);
  }

  {
    const auto &test_data = solver_test.bend_twist;

    EXPECT_EIGEN_V3_DIAG_NEAR(
        compliance_v(test_data.alphas[0], test_data.betas[0]), H.block(23, 23, 3, 3), eps);
    EXPECT_EIGEN_V3_DIAG_NEAR(
        compliance_v(test_data.alphas[1], test_data.betas[1]), H.block(26, 26, 3, 3), eps);

    float3 residual[2];
    float4x4 rot_gradient1[2], rot_gradient2[2];
    xpbd_constraints::eval_bend_twist_elements(test_data.darboux_vector[0],
                                               solver_test.vars.rotations[test_data.point1[0]],
                                               solver_test.vars.rotations[test_data.point2[0]],
                                               residual[0],
                                               rot_gradient1[0],
                                               rot_gradient2[0]);
    xpbd_constraints::eval_bend_twist_elements(test_data.darboux_vector[1],
                                               solver_test.vars.rotations[test_data.point1[1]],
                                               solver_test.vars.rotations[test_data.point2[1]],
                                               residual[1],
                                               rot_gradient1[1],
                                               rot_gradient2[1]);
    EXPECT_EIGEN_MATRIX_NEAR(math::transpose(rot_gradient1[0]), H.block(23, 13, 3, 4), eps);
    EXPECT_EIGEN_MATRIX_NEAR(rot_gradient1[0], H.block(13, 23, 4, 3), eps);
    EXPECT_EIGEN_MATRIX_NEAR(math::transpose(rot_gradient2[0]), H.block(23, 17, 3, 4), eps);
    EXPECT_EIGEN_MATRIX_NEAR(rot_gradient2[0], H.block(17, 23, 4, 3), eps);
    EXPECT_EIGEN_MATRIX_NEAR(math::transpose(rot_gradient1[1]), H.block(26, 9, 3, 4), eps);
    EXPECT_EIGEN_MATRIX_NEAR(rot_gradient1[1], H.block(9, 26, 4, 3), eps);
    EXPECT_EIGEN_MATRIX_NEAR(math::transpose(rot_gradient2[1]), H.block(26, 13, 3, 4), eps);
    EXPECT_EIGEN_MATRIX_NEAR(rot_gradient2[1], H.block(13, 26, 4, 3), eps);

    /* Constraint lambda residuals. */
    const float3 target0 =
        target_residual_v(
            residual[0], test_data.alphas[0], test_data.betas[0], test_data.lambdas[0]) +
        target_angular_velocity_v(
            test_data.alphas[0], test_data.betas[0], test_data.point1[0], rot_gradient1[0]) +
        target_angular_velocity_v(
            test_data.alphas[0], test_data.betas[0], test_data.point2[0], rot_gradient2[0]);
    const float3 target1 =
        target_residual_v(
            residual[1], test_data.alphas[1], test_data.betas[1], test_data.lambdas[1]) +
        target_angular_velocity_v(
            test_data.alphas[1], test_data.betas[1], test_data.point1[1], rot_gradient1[1]) +
        target_angular_velocity_v(
            test_data.alphas[1], test_data.betas[1], test_data.point2[1], rot_gradient2[1]);
    EXPECT_NEAR(target0.x, b[23], eps);
    EXPECT_NEAR(target0.y, b[24], eps);
    EXPECT_NEAR(target0.z, b[25], eps);
    EXPECT_NEAR(target1.x, b[26], eps);
    EXPECT_NEAR(target1.y, b[27], eps);
    EXPECT_NEAR(target1.z, b[28], eps);
  }
}

TEST_F(XPBDSolverTest, GlobalSolverExecute)
{
  // constexpr float eps = 1e-6f;

  SolverTestData solver_test = simple_solver_data(false);

  IndexMaskMemory memory;
  xpbd_constraints::GlobalSolverSystem system = xpbd_constraints::build_global_solve_system(
      solver_test.params, solver_test.data, solver_test.vars, true, memory);

  xpbd_constraints::solve_global_system(system, solver_test.vars, solver_test.data);
}

}  // namespace blender::nodes::tests
