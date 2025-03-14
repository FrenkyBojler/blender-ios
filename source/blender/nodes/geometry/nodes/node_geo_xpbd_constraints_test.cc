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

namespace blender::nodes::tests {

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
  const math::Quaternion darboux_vector = math::Quaternion(
      float4(math::invert(target_rotation1) * target_rotation2));
  EXPECT_V4_NEAR(float4(x, x, 0, 0), float4(darboux_vector), 1.e-5f);

  const float alpha = 0.0f;

  /* Rotation 1 only. */
  {
    float4 lambda = float4(0.0f);
    math::Quaternion rotation1 = math::Quaternion::identity();
    math::Quaternion rotation2 = math::Quaternion::identity();
    xpbd_constraints::apply_position_bend_twist<true>(
        1, 0, darboux_vector, alpha, lambda, rotation1, rotation2);
    EXPECT_V4_NEAR(float4(0.0f, x, 0, 0), lambda, 1e-5f);
    EXPECT_V4_NEAR(math::normalize(float4(1.0f, -x, 0, 0)), float4(rotation1), 1e-5f);
    EXPECT_V4_NEAR(float4(1, 0, 0, 0), float4(rotation2), 1e-5f);
  }
  /* Rotation 2 only. */
  {
    float4 lambda = float4(0.0f);
    math::Quaternion rotation1 = math::Quaternion::identity();
    math::Quaternion rotation2 = math::Quaternion::identity();
    xpbd_constraints::apply_position_bend_twist<true>(
        0, 1, darboux_vector, alpha, lambda, rotation1, rotation2);
    EXPECT_V4_NEAR(float4(0.0f, x, 0, 0), lambda, 1e-5f);
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

static auto simple_solver_data(const bool use_velocities)
{
  xpbd_constraints::ConstraintEvalParams params;
  params.masses = VArray<float>::ForContainer(Array<float>{1.0f, 3.0f, 0.5f});
  params.local_inertia = VArray<float3>::ForContainer(
      Array<float3>{float3(1.0f), float3(1, 2, 1), float3(0.2f, 10.f, 0.5f)});

  xpbd_constraints::ConstraintVariables vars;
  vars.positions = {float3(0, 1, 0), float3(1, 0, 0), float3(0, 0, -2)};
  vars.rotations = {math::to_quaternion(math::EulerXYZ(0, 0, 0)),
                    math::to_quaternion(math::EulerXYZ(90, 0, 0)),
                    math::to_quaternion(math::EulerXYZ(0, -20, 0))};
  if (use_velocities) {
    vars.velocities = {float3(-1, -1, 0), float3(0, 0, 0), float3(0, 0, 4)};
    vars.angular_velocities = {float3(0, 0, 0), float3(3, 0, -1), float3(2, 2, 0)};
  }

  Array<xpbd_constraints::ConstraintEvalData> data(1);

  data[0].type = &xpbd_constraints::get_info__position_goal(true);
  PointCloud *constraints = BKE_pointcloud_new_nomain(2);
  bke::MutableAttributeAccessor attributes = constraints->attributes_for_write();

  const Array<int> point1 = {0, 2};
  const Array<float3> goal_position = {float3(0.0f), float3(1, -1, 2)};
  const Array<float> alphas = {1.5f, 0.1f};
  const Array<float> betas = {0.3f, 0.0f};
  attributes.add<int>(
      "point1", bke::AttrDomain::Point, bke::AttributeInitVArray(VArray<int>::ForSpan(point1)));
  attributes.add<float3>("goal_position",
                         bke::AttrDomain::Point,
                         bke::AttributeInitVArray(VArray<float3>::ForSpan(goal_position)));
  attributes.add<float>("compliance",
                        bke::AttrDomain::Point,
                        bke::AttributeInitVArray(VArray<float>::ForSpan(alphas)));
  attributes.add<float>(
      "damping", bke::AttrDomain::Point, bke::AttributeInitVArray(VArray<float>::ForSpan(betas)));

  data[0].geometry = bke::GeometrySet::from_pointcloud(std::move(constraints));
  data[0].constraints = IndexRange(attributes.domain_size(bke::AttrDomain::Point));

  return std::make_tuple(std::move(params), std::move(data), std::move(vars));
}

inline float4x4 quaternion_matrix(const math::Quaternion &q)
{
  float4x4 result;
  result[0] = float4{q.w, q.x, q.y, q.z};
  result[1] = float4{-q.x, q.x, -q.z, q.y};
  result[1] = float4{-q.y, q.z, q.y, -q.x};
  result[1] = float4{-q.z, -q.y, q.x, q.z};
  return result;
}

#define EXPECT_EIGEN_M3_NEAR(a, b, eps) \
  do { \
    EXPECT_NEAR(a[0][0], b.coeff(0, 0), eps); \
    EXPECT_NEAR(a[0][1], b.coeff(1, 0), eps); \
    EXPECT_NEAR(a[0][2], b.coeff(2, 0), eps); \
    EXPECT_NEAR(a[1][0], b.coeff(0, 1), eps); \
    EXPECT_NEAR(a[1][1], b.coeff(1, 1), eps); \
    EXPECT_NEAR(a[1][2], b.coeff(2, 1), eps); \
    EXPECT_NEAR(a[2][0], b.coeff(0, 2), eps); \
    EXPECT_NEAR(a[2][1], b.coeff(1, 2), eps); \
    EXPECT_NEAR(a[2][2], b.coeff(2, 2), eps); \
  } while (false);

#define EXPECT_EIGEN_M4_NEAR(a, b, eps) \
  do { \
    EXPECT_NEAR(a[0][0], b.coeff(0, 0), eps); \
    EXPECT_NEAR(a[0][1], b.coeff(1, 0), eps); \
    EXPECT_NEAR(a[0][2], b.coeff(2, 0), eps); \
    EXPECT_NEAR(a[0][3], b.coeff(3, 0), eps); \
    EXPECT_NEAR(a[1][0], b.coeff(0, 1), eps); \
    EXPECT_NEAR(a[1][1], b.coeff(1, 1), eps); \
    EXPECT_NEAR(a[1][2], b.coeff(2, 1), eps); \
    EXPECT_NEAR(a[1][3], b.coeff(3, 1), eps); \
    EXPECT_NEAR(a[2][0], b.coeff(0, 2), eps); \
    EXPECT_NEAR(a[2][1], b.coeff(1, 2), eps); \
    EXPECT_NEAR(a[2][2], b.coeff(2, 2), eps); \
    EXPECT_NEAR(a[2][3], b.coeff(3, 2), eps); \
    EXPECT_NEAR(a[3][0], b.coeff(0, 3), eps); \
    EXPECT_NEAR(a[3][1], b.coeff(1, 3), eps); \
    EXPECT_NEAR(a[3][2], b.coeff(2, 3), eps); \
    EXPECT_NEAR(a[3][3], b.coeff(3, 3), eps); \
  } while (false);

TEST_F(XPBDSolverTest, GlobalSolverUnconstrained)
{
  constexpr float eps = 1e-6f;

  auto [params, data, vars] = simple_solver_data(false);

  Eigen::SparseMatrix<float> H;
  Eigen::VectorXf b;
  xpbd_constraints::build_global_solve_system(params, data, vars, true, H, b);
  EXPECT_EQ(H.rows(), 23);
  EXPECT_EQ(H.cols(), 23);
  // XXX only counting compliance values atm
  EXPECT_EQ(H.nonZeros(), 59);
  // EXPECT_EQ(H.nonZeros(), 71);

  const float3x3 mass_diagonal0 = math::from_scale<float3x3>(float3(params.masses[0]));
  const float3x3 mass_diagonal1 = math::from_scale<float3x3>(float3(params.masses[1]));
  const float3x3 mass_diagonal2 = math::from_scale<float3x3>(float3(params.masses[2]));
  EXPECT_EIGEN_M3_NEAR(mass_diagonal0, H.block(0, 0, 3, 3).toDense(), eps);
  EXPECT_EIGEN_M3_NEAR(mass_diagonal1, H.block(3, 3, 3, 3).toDense(), eps);
  EXPECT_EIGEN_M3_NEAR(mass_diagonal2, H.block(6, 6, 3, 3).toDense(), eps);

  const float4x4 inertia_tensor0 = quaternion_matrix(
      vars.rotations[0] * math::Quaternion(0.0f, params.local_inertia[0]));
  const float4x4 inertia_tensor1 = quaternion_matrix(
      vars.rotations[1] * math::Quaternion(0.0f, params.local_inertia[1]));
  const float4x4 inertia_tensor2 = quaternion_matrix(
      vars.rotations[2] * math::Quaternion(0.0f, params.local_inertia[2]));
  EXPECT_EIGEN_M4_NEAR(inertia_tensor0, H.block(9, 9, 4, 4).toDense(), eps);
  EXPECT_EIGEN_M4_NEAR(inertia_tensor1, H.block(13, 13, 4, 4).toDense(), eps);
  EXPECT_EIGEN_M4_NEAR(inertia_tensor2, H.block(17, 17, 4, 4).toDense(), eps);

  const Array<float> alphas = {1.5f, 0.1f};
  const Array<float> betas = {0.3f, 0.0f};
  EXPECT_NEAR(1.0f + alphas[0], H.coeff(21, 21), eps);
}

}  // namespace blender::nodes::tests
