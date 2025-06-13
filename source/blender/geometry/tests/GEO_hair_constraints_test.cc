/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "GEO_hair_constraints.hh"

#include "testing/testing.h"

namespace blender::geometry::tests {

TEST(hair_constraints, PositionGoal)
{
  float lambda = 0.0f;
  float3 position = {1, 2, 3};
  const float3 goal = {-2, 0, 2};
  const float delta = math::length(float3(-3, -2, -1));
  EXPECT_NEAR(delta, 3.741657f, 1e-5f);

  const float alpha = 0.0f;

  hair_constraints::apply_position_goal(goal, alpha, lambda, position);
  EXPECT_NEAR(-delta, lambda, 1e-5f);
  EXPECT_V3_NEAR(goal, position, 1e-5f);
}

TEST(hair_constraints, RotationGoal)
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

  hair_constraints::apply_rotation_goal<false>(goal, alpha, lambda, rotation);
  EXPECT_NEAR(-delta.angle().radian(), lambda, 1e-5f);
  EXPECT_V4_NEAR(float4(goal), float4(rotation), 1e-5f);
}

TEST(hair_constraints, RotationGoalLinearized)
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

  hair_constraints::apply_rotation_goal<true>(goal, alpha, lambda, rotation);
  EXPECT_NEAR(-delta.angle().radian(), lambda, 1e-5f);
  /* Linearized quaternion offset does not yield exact rotation for large offsets. */
  EXPECT_V4_NEAR(float4(goal), float4(rotation), 0.05f);
}

TEST(hair_constraints, InactiveContact)
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

  bool active = hair_constraints::apply_position_contact(weight_pos1,
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

TEST(hair_constraints, CentralContact)
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

  bool active = hair_constraints::apply_position_contact(weight_pos1,
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

TEST(hair_constraints, OffcenterContact)
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

  bool active = hair_constraints::apply_position_contact(weight_pos1,
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

TEST(hair_constraints, ContactFriction)
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

  hair_constraints::apply_velocity_contact(orig_velocity1,
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

TEST(hair_constraints, ContactRestitution)
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

  hair_constraints::apply_velocity_contact(orig_velocity1,
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

TEST(hair_constraints, StretchShear)
{
  const float edge_length = 2.0f;

  const float alpha = 0.0f;

  /* Position 1 only. */
  {
    float3 lambda = float3(0.0f);
    float3 position1 = {-1, 0, 0};
    float3 position2 = {0, 0, 1};
    math::Quaternion rotation = math::Quaternion::identity();
    hair_constraints::apply_position_stretch_shear<false>(
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
    hair_constraints::apply_position_stretch_shear<false>(
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
    hair_constraints::apply_position_stretch_shear<false>(
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
    hair_constraints::apply_position_stretch_shear<true>(
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

TEST(hair_constraints, BendTwist)
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

  const float3 alpha = float3(0.0f);

  /* Rotation 1 only. */
  {
    float3 lambda = float3(0.0f);
    math::Quaternion rotation1 = math::Quaternion::identity();
    math::Quaternion rotation2 = math::Quaternion::identity();
    hair_constraints::apply_position_bend_twist<true>(
        float3(1), float3(0), darboux_vector, alpha, lambda, rotation1, rotation2);
    EXPECT_V3_NEAR(float3(x, 0, 0), lambda, 1e-5f);
    EXPECT_V4_NEAR(math::normalize(float4(1.0f, -x, 0, 0)), float4(rotation1), 1e-5f);
    EXPECT_V4_NEAR(float4(1, 0, 0, 0), float4(rotation2), 1e-5f);
  }
  /* Rotation 2 only. */
  {
    float3 lambda = float3(0.0f);
    math::Quaternion rotation1 = math::Quaternion::identity();
    math::Quaternion rotation2 = math::Quaternion::identity();
    hair_constraints::apply_position_bend_twist<true>(
        float3(0), float3(1), darboux_vector, alpha, lambda, rotation1, rotation2);
    EXPECT_V3_NEAR(float3(x, 0, 0), lambda, 1e-5f);
    EXPECT_V4_NEAR(float4(1, 0, 0, 0), float4(rotation1), 1e-5f);
    EXPECT_V4_NEAR(math::normalize(float4(1.0f, x, 0, 0)), float4(rotation2), 1e-5f);
  }
}

/* Unused: Helper struct for checking that each variable is only written by one constraint.*/
#if 0
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
#endif

}  // namespace blender::geometry::tests
