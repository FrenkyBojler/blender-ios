/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_rotation.hh"

#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_pointcloud.hh"

#include "GEO_hair_constraint_functions.hh"

#include "NOD_geo_hair_constraints.hh"
#include "NOD_xpbd_solver.hh"

#include "CLG_log.h"

#include "testing/testing.h"

#include <iostream>

namespace blender::nodes::tests {

using geometry::hair_constraints::ConstraintEvalParams;
using geometry::hair_constraints::ConstraintType;
using geometry::hair_constraints::ConstraintTypeInfo;
using geometry::hair_constraints::ConstraintVariables;
using xpbd_constraints::ConstraintEvalData;

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

struct SolverTestData {
  ConstraintEvalParams params;
  ConstraintVariables vars;
  Vector<ConstraintEvalData> data;

  /* These are the same arrays stored in point cloud attributes,
   * put here directly for convenient testing. */
  Array<float4x4> collider_transforms;
  Array<float4x4> old_collider_transforms;
  struct {
    Array<int> point1;
    Array<float> lambdas;
    Array<float> alphas;
    Array<float> betas;
    Array<float3> goal_position;
  } position_goal;
  struct {
    Array<int> point1;
    Array<float3> lambdas;
    Array<float3> alphas;
    Array<float3> betas;
    Array<math::Quaternion> goal_rotation;
  } rotation_goal;
  struct {
    Array<int> point1;
    Array<int> point2;
    Array<float3> lambdas;
    Array<float3> alphas;
    Array<float3> betas;
    Array<float> edge_lengths;
  } stretch_shear;
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

static SolverTestData simple_solver_data(const Span<ConstraintType> constraint_types,
                                         const bool use_velocities)
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
  solver_test.collider_transforms = {
      math::from_loc_rot<float4x4>(float3(0, 0, 1), math::to_quaternion(math::EulerXYZ(0, 0, 0))),
      math::from_loc_rot<float4x4>(float3(-3, -2, 0),
                                   math::to_quaternion(math::EulerXYZ(0, -30, 75)))};
  solver_test.old_collider_transforms = {
      math::from_loc_rot<float4x4>(float3(0, 0, 1.5),
                                   math::to_quaternion(math::EulerXYZ(0, 20, 0))),
      math::from_loc_rot<float4x4>(float3(-3, -2, 0),
                                   math::to_quaternion(math::EulerXYZ(20, -10, 50)))};
  solver_test.params.collider_transforms = solver_test.collider_transforms;
  solver_test.params.old_collider_transforms = solver_test.old_collider_transforms;

  solver_test.vars.positions = {float3(0, 1, 0), float3(1, 0, 0), float3(0, 0, -2)};
  solver_test.vars.rotations = {math::to_quaternion(math::EulerXYZ(0, 0, 0)),
                                math::to_quaternion(math::EulerXYZ(90, 0, 0)),
                                math::to_quaternion(math::EulerXYZ(0, -20, 0))};
  if (use_velocities) {
    solver_test.vars.velocities = {float3(-1, -1, 0), float3(0, 0, 0), float3(0, 0, 4)};
    solver_test.vars.angular_velocities = {float3(0, 0, 0), float3(3, 0, -1), float3(2, 2, 0)};
  }

  using AttributeInfo = std::pair<StringRef, GSpan>;
  auto add_constraint_data = [&](const ConstraintTypeInfo &type,
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

  if (constraint_types.contains(ConstraintType::PositionGoal)) {
    solver_test.position_goal.point1 = {0, 2};
    solver_test.position_goal.lambdas = {0.2f, 3.0f};
    solver_test.position_goal.alphas = {1.5f, 0.1f};
    solver_test.position_goal.betas = {0.3f, 0.0f};
    solver_test.position_goal.goal_position = {float3(0.0f), float3(1, -1, 2)};

    add_constraint_data(
        geometry::hair_constraints::get_info(ConstraintType::PositionGoal, true),
        {AttributeInfo{"point1", solver_test.position_goal.point1.as_span()},
         AttributeInfo{"lambda", solver_test.position_goal.lambdas.as_span()},
         AttributeInfo{"goal_position", solver_test.position_goal.goal_position.as_span()},
         AttributeInfo{"compliance", solver_test.position_goal.alphas.as_span()},
         AttributeInfo{"damping", solver_test.position_goal.betas.as_span()}});
  }
  if (constraint_types.contains(ConstraintType::RotationGoal)) {
    solver_test.rotation_goal.point1 = {1, 2};
    solver_test.rotation_goal.lambdas = {float3(0.0f, 0.5f, 1.0f), float3(0.01f, 0.0f, 0.9f)};
    solver_test.rotation_goal.alphas = {float3(0.4f, 0.1f, 0.4f), float3(0.2f, 0.0f, 1.0f)};
    solver_test.rotation_goal.betas = {float3(1.3f, 0.01f, 0.0f), float3(0.0f, 0.0f, 1.0f)};
    solver_test.rotation_goal.goal_rotation = {math::to_quaternion(math::EulerXYZ(-140, 20, 0)),
                                               math::to_quaternion(math::EulerXYZ(0, 10, 10))};

    add_constraint_data(
        geometry::hair_constraints::get_info(ConstraintType::RotationGoal, true),
        {AttributeInfo{"point1", solver_test.rotation_goal.point1.as_span()},
         AttributeInfo{"lambda", solver_test.rotation_goal.lambdas.as_span()},
         AttributeInfo{"goal_rotation", solver_test.rotation_goal.goal_rotation.as_span()},
         AttributeInfo{"compliance", solver_test.rotation_goal.alphas.as_span()},
         AttributeInfo{"damping", solver_test.rotation_goal.betas.as_span()}});
  }
  if (constraint_types.contains(ConstraintType::StretchShear)) {
    solver_test.stretch_shear.point1 = {1, 0};
    solver_test.stretch_shear.point2 = {2, 1};
    solver_test.stretch_shear.lambdas = {float3(-1.0f, 0.0f, 0.0f), float3(4.0f, -4.0f, 1.0f)};
    solver_test.stretch_shear.alphas = {float3(0.6f, 1.1f, 0.0f), float3(0.0f, 0.0f, 3.0f)};
    solver_test.stretch_shear.betas = {float3(0.5f, 0.001f, 0.5f), float3(1.0f, 1.0f, 1.0f)};
    solver_test.stretch_shear.edge_lengths = {0.8f, 2.5f};

    add_constraint_data(
        geometry::hair_constraints::get_info(ConstraintType::StretchShear, true),
        {AttributeInfo{"point1", solver_test.stretch_shear.point1.as_span()},
         AttributeInfo{"point2", solver_test.stretch_shear.point2.as_span()},
         AttributeInfo{"lambda", solver_test.stretch_shear.lambdas.as_span()},
         AttributeInfo{"edge_length", solver_test.stretch_shear.edge_lengths.as_span()},
         AttributeInfo{"compliance", solver_test.stretch_shear.alphas.as_span()},
         AttributeInfo{"damping", solver_test.stretch_shear.betas.as_span()}});
  }
  if (constraint_types.contains(ConstraintType::BendTwist)) {
    solver_test.bend_twist.point1 = {1, 0};
    solver_test.bend_twist.point2 = {2, 1};
    solver_test.bend_twist.lambdas = {float3(-1.0f, 0.0f, 0.0f), float3(4.0f, -4.0f, 1.0f)};
    solver_test.bend_twist.alphas = {float3(0.6f, 1.1f, 0.0f), float3(0.0f, 0.0f, 3.0f)};
    solver_test.bend_twist.betas = {float3(0.5f, 0.001f, 0.5f), float3(1.0f, 1.0f, 1.0f)};
    solver_test.bend_twist.darboux_vector = {float3(0.2f, 0.8f, 1.1f), float3(-0.5f, -0.5f, 2.2f)};

    add_constraint_data(
        geometry::hair_constraints::get_info(ConstraintType::BendTwist, true),
        {AttributeInfo{"point1", solver_test.bend_twist.point1.as_span()},
         AttributeInfo{"point2", solver_test.bend_twist.point2.as_span()},
         AttributeInfo{"lambda", solver_test.bend_twist.lambdas.as_span()},
         AttributeInfo{"darboux_vector", solver_test.bend_twist.darboux_vector.as_span()},
         AttributeInfo{"compliance", solver_test.bend_twist.alphas.as_span()},
         AttributeInfo{"damping", solver_test.bend_twist.betas.as_span()}});
  }
  if (constraint_types.contains(ConstraintType::Contact)) {
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
        float3(0.0f, 0.0f, -1.0f), float3(0.3f, 0.4f, 1.0f), float3(0.0f, 0.0f, 1.0f)};

    add_constraint_data(
        geometry::hair_constraints::get_info(ConstraintType::Contact, true),
        {AttributeInfo{"point1", solver_test.contact.point1.as_span()},
         AttributeInfo{"collider_index", solver_test.contact.collider_index.as_span()},
         AttributeInfo{"lambda", solver_test.contact.lambdas.as_span()},
         AttributeInfo{"compliance", solver_test.contact.alphas.as_span()},
         AttributeInfo{"damping", solver_test.contact.betas.as_span()},
         AttributeInfo{"local_position1", solver_test.contact.local_position1.as_span()},
         AttributeInfo{"local_position2", solver_test.contact.local_position2.as_span()},
         AttributeInfo{"normal", solver_test.contact.normal.as_span()}});
  }

  return solver_test;
}

inline float trace(const float3 &v)
{
  return v.x + v.y + v.z;
}

/* Expected values for compliance entries. */
inline float compliance(const SolverTestData &solver_test, const float alpha, const float beta)
{
  return alpha * solver_test.params.inv_delta_time_squared / (1.0f + alpha * beta);
}

inline float3 compliance(const SolverTestData &solver_test,
                         const float3 &alpha,
                         const float3 &beta)
{
  return alpha * solver_test.params.inv_delta_time_squared / (float3(1.0f) + alpha * beta);
}

inline float target_residual(const SolverTestData &solver_test,
                             const float residual,
                             const float alpha,
                             const float beta,
                             const float lambda)
{
  return (residual + alpha * lambda * solver_test.params.inv_delta_time_squared) /
         (1.0f + alpha * beta);
}

inline float3 target_residual(const SolverTestData &solver_test,
                              const float3 &residual,
                              const float3 &alpha,
                              const float3 &beta,
                              const float3 &lambda)
{
  return (residual + alpha * lambda * solver_test.params.inv_delta_time_squared) /
         (float3(1.0f) + alpha * beta);
}

inline float target_velocity(const SolverTestData &solver_test,
                             const float alpha,
                             const float beta,
                             const int point_index,
                             const float3 &gradient)
{
  const float3 velocity = (solver_test.vars.positions[point_index] -
                           solver_test.params.old_positions[point_index]) *
                          solver_test.params.inv_delta_time;
  return (alpha * beta * math::dot(gradient, velocity)) / (1.0f + alpha * beta);
}
inline float3 target_velocity(const SolverTestData &solver_test,
                              const float3 &alpha,
                              const float3 &beta,
                              const int point_index,
                              const float4x4 &gradient)
{
  const float3 velocity = (solver_test.vars.positions[point_index] -
                           solver_test.params.old_positions[point_index]) *
                          solver_test.params.inv_delta_time;
  return (alpha * beta * (velocity * gradient.view<3, 3>())) / (1.0f + alpha * beta);
}

inline float target_angular_velocity(const SolverTestData &solver_test,
                                     const float alpha,
                                     const float beta,
                                     const int point_index,
                                     const float4 &gradient)
{
  const float4 velocity = (float4(solver_test.vars.rotations[point_index]) -
                           float4(solver_test.params.old_rotations[point_index])) *
                          solver_test.params.inv_delta_time;
  /* Note: gradient is actually transpose of the Jacobian, each column is the derivative of
   * one constraint variable. */
  return (alpha * beta * math::dot(gradient, velocity)) / (1.0f + alpha * beta);
}

inline float3 target_angular_velocity(const SolverTestData &solver_test,
                                      const float3 &alpha,
                                      const float3 &beta,
                                      const int point_index,
                                      const float4x4 &gradient)
{
  const float4 velocity = (float4(solver_test.vars.rotations[point_index]) -
                           float4(solver_test.params.old_rotations[point_index])) *
                          solver_test.params.inv_delta_time;
  /* Note: gradient is actually transpose of the Jacobian, each column is the derivative of
   * one constraint variable. */
  return (alpha * beta * (velocity * gradient.view<3, 4>())) / (float3(1.0f) + alpha * beta);
}

TEST_F(XPBDSolverTest, GlobalSolverUnconstrained)
{
  constexpr float eps = 1e-6f;

  SolverTestData solver_test = simple_solver_data({}, false);

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
  EXPECT_EQ(21, H.rows());
  EXPECT_EQ(21, H.cols());
  EXPECT_EQ(21, H.nonZeros());
  EXPECT_EQ(21, b.rows());
  EXPECT_TRUE(system.constraint_mapping.is_empty());

  EXPECT_EIGEN_V3_DIAG_NEAR(float3(solver_test.params.masses[0]), H.block(0, 0, 3, 3), eps);
  EXPECT_EIGEN_V3_DIAG_NEAR(float3(solver_test.params.masses[1]), H.block(3, 3, 3, 3), eps);
  EXPECT_EIGEN_V3_DIAG_NEAR(float3(solver_test.params.masses[2]), H.block(6, 6, 3, 3), eps);

  const float4 inertia_diag0 = {0.5f * trace(solver_test.params.local_inertia[0]),
                                solver_test.params.local_inertia[0]};
  const float4 inertia_diag1 = {0.5f * trace(solver_test.params.local_inertia[1]),
                                solver_test.params.local_inertia[1]};
  const float4 inertia_diag2 = {0.5f * trace(solver_test.params.local_inertia[2]),
                                solver_test.params.local_inertia[2]};
  EXPECT_EIGEN_V4_DIAG_NEAR(inertia_diag0, H.block(9, 9, 4, 4), eps);
  EXPECT_EIGEN_V4_DIAG_NEAR(inertia_diag1, H.block(13, 13, 4, 4), eps);
  EXPECT_EIGEN_V4_DIAG_NEAR(inertia_diag2, H.block(17, 17, 4, 4), eps);

  /* Residual for motion equations should be zero. */
  for (const int i : IndexRange(21)) {
    EXPECT_EQ(0.0f, b[i]);
  }
}

TEST_F(XPBDSolverTest, GlobalSolverConstraints_PositionGoal)
{
  constexpr float eps = 1e-6f;

  SolverTestData solver_test = simple_solver_data({ConstraintType::PositionGoal}, false);
  const auto &test_data = solver_test.position_goal;

  IndexMaskMemory memory;
  xpbd_constraints::GlobalSolverSystem system = xpbd_constraints::build_global_solve_system(
      solver_test.params, solver_test.data, solver_test.vars, true, memory);
  const Eigen::SparseMatrix<float> &H = system.matrix;
  const Eigen::VectorXf &b = system.target;
  EXPECT_EQ(23, H.rows());
  EXPECT_EQ(23, H.cols());
  EXPECT_EQ(35, H.nonZeros());
  EXPECT_EQ(23, b.rows());
  EXPECT_EQ(system.constraint_mapping.size(), 1);

  EXPECT_NEAR(
      -compliance(solver_test, test_data.alphas[0], test_data.betas[0]), H.coeff(21, 21), eps);
  EXPECT_NEAR(
      -compliance(solver_test, test_data.alphas[1], test_data.betas[1]), H.coeff(22, 22), eps);

  float residual[2];
  float3 pos_gradient[2];
  geometry::hair_constraints::eval_position_goal_elements(
      test_data.goal_position[0],
      solver_test.vars.positions[test_data.point1[0]],
      residual[0],
      pos_gradient[0]);
  geometry::hair_constraints::eval_position_goal_elements(
      test_data.goal_position[1],
      solver_test.vars.positions[test_data.point1[1]],
      residual[1],
      pos_gradient[1]);
  EXPECT_EIGEN_V3_ROW_NEAR(-pos_gradient[0], H.block(21, 0, 1, 3), eps);
  EXPECT_EIGEN_V3_COL_NEAR(-pos_gradient[0], H.block(0, 21, 3, 1), eps);
  EXPECT_EIGEN_V3_ROW_NEAR(-pos_gradient[1], H.block(22, 6, 1, 3), eps);
  EXPECT_EIGEN_V3_COL_NEAR(-pos_gradient[1], H.block(6, 22, 3, 1), eps);

  /* Constraint lambda residuals. */
  const float target0 = target_residual(solver_test,
                                        residual[0],
                                        test_data.alphas[0],
                                        test_data.betas[0],
                                        test_data.lambdas[0]) +
                        target_velocity(solver_test,
                                        test_data.alphas[0],
                                        test_data.betas[0],
                                        test_data.point1[0],
                                        pos_gradient[0]);
  const float target1 = target_residual(solver_test,
                                        residual[1],
                                        test_data.alphas[1],
                                        test_data.betas[1],
                                        test_data.lambdas[1]) +
                        target_velocity(solver_test,
                                        test_data.alphas[1],
                                        test_data.betas[1],
                                        test_data.point1[1],
                                        pos_gradient[1]);
  EXPECT_NEAR(target0, b[21], eps);
  EXPECT_NEAR(target1, b[22], eps);

  EXPECT_EQ(system.constraint_mapping[0].size(), 2);
  EXPECT_EQ(system.constraint_mapping[0][0], 0);
  EXPECT_EQ(system.constraint_mapping[0][1], 1);
}

TEST_F(XPBDSolverTest, GlobalSolverConstraints_RotationGoal)
{
  constexpr float eps = 1e-6f;

  SolverTestData solver_test = simple_solver_data({ConstraintType::RotationGoal}, false);
  const auto &test_data = solver_test.rotation_goal;

  IndexMaskMemory memory;
  xpbd_constraints::GlobalSolverSystem system = xpbd_constraints::build_global_solve_system(
      solver_test.params, solver_test.data, solver_test.vars, true, memory);
  const Eigen::SparseMatrix<float> &H = system.matrix;
  const Eigen::VectorXf &b = system.target;
  EXPECT_EQ(27, H.rows());
  EXPECT_EQ(27, H.cols());
  EXPECT_EQ(75, H.nonZeros());
  EXPECT_EQ(27, b.rows());
  EXPECT_EQ(system.constraint_mapping.size(), 1);

  EXPECT_EIGEN_V3_DIAG_NEAR(-compliance(solver_test, test_data.alphas[0], test_data.betas[0]),
                            H.block(21, 21, 3, 3),
                            eps);
  EXPECT_EIGEN_V3_DIAG_NEAR(-compliance(solver_test, test_data.alphas[1], test_data.betas[1]),
                            H.block(24, 24, 3, 3),
                            eps);

  float3 residual[2];
  float4x4 rot_gradient1[2];
  geometry::hair_constraints::eval_rotation_goal_elements(
      test_data.goal_rotation[0],
      solver_test.vars.rotations[test_data.point1[0]],
      residual[0],
      rot_gradient1[0]);
  geometry::hair_constraints::eval_rotation_goal_elements(
      test_data.goal_rotation[1],
      solver_test.vars.rotations[test_data.point1[1]],
      residual[1],
      rot_gradient1[1]);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(rot_gradient1[0]), H.block(21, 13, 3, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-rot_gradient1[0], H.block(13, 21, 4, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(rot_gradient1[1]), H.block(24, 17, 3, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-rot_gradient1[1], H.block(17, 24, 4, 3), eps);

  /* Constraint lambda residuals. */
  const float3 target0 = target_residual(solver_test,
                                         residual[0],
                                         test_data.alphas[0],
                                         test_data.betas[0],
                                         test_data.lambdas[0]) +
                         target_angular_velocity(solver_test,
                                                 test_data.alphas[0],
                                                 test_data.betas[0],
                                                 test_data.point1[0],
                                                 rot_gradient1[0]);
  const float3 target1 = target_residual(solver_test,
                                         residual[1],
                                         test_data.alphas[1],
                                         test_data.betas[1],
                                         test_data.lambdas[1]) +
                         target_angular_velocity(solver_test,
                                                 test_data.alphas[1],
                                                 test_data.betas[1],
                                                 test_data.point1[1],
                                                 rot_gradient1[1]);
  EXPECT_NEAR(target0.x, b[21], eps);
  EXPECT_NEAR(target0.y, b[22], eps);
  EXPECT_NEAR(target0.z, b[23], eps);
  EXPECT_NEAR(target1.x, b[24], eps);
  EXPECT_NEAR(target1.y, b[25], eps);
  EXPECT_NEAR(target1.z, b[26], eps);

  EXPECT_EQ(system.constraint_mapping[0].size(), 2);
  EXPECT_EQ(system.constraint_mapping[0][0], 0);
  EXPECT_EQ(system.constraint_mapping[0][1], 1);
}

TEST_F(XPBDSolverTest, GlobalSolverConstraints_StretchShear)
{
  constexpr float eps = 1e-6f;

  SolverTestData solver_test = simple_solver_data({ConstraintType::StretchShear}, false);
  const auto &test_data = solver_test.stretch_shear;

  IndexMaskMemory memory;
  xpbd_constraints::GlobalSolverSystem system = xpbd_constraints::build_global_solve_system(
      solver_test.params, solver_test.data, solver_test.vars, true, memory);
  const Eigen::SparseMatrix<float> &H = system.matrix;
  const Eigen::VectorXf &b = system.target;
  EXPECT_EQ(27, H.rows());
  EXPECT_EQ(27, H.cols());
  EXPECT_EQ(147, H.nonZeros());
  EXPECT_EQ(27, b.rows());
  EXPECT_EQ(system.constraint_mapping.size(), 1);

  EXPECT_EIGEN_V3_DIAG_NEAR(-compliance(solver_test, test_data.alphas[0], test_data.betas[0]),
                            H.block(21, 21, 3, 3),
                            eps);
  EXPECT_EIGEN_V3_DIAG_NEAR(-compliance(solver_test, test_data.alphas[1], test_data.betas[1]),
                            H.block(24, 24, 3, 3),
                            eps);

  float3 residual[2];
  float4x4 pos_gradient1[2], pos_gradient2[2], rot_gradient[2];
  geometry::hair_constraints::eval_stretch_shear_elements(
      test_data.edge_lengths[0],
      solver_test.vars.positions[test_data.point1[0]],
      solver_test.vars.positions[test_data.point2[0]],
      solver_test.vars.rotations[test_data.point1[0]],
      residual[0],
      pos_gradient1[0],
      pos_gradient2[0],
      rot_gradient[0]);
  geometry::hair_constraints::eval_stretch_shear_elements(
      test_data.edge_lengths[1],
      solver_test.vars.positions[test_data.point1[1]],
      solver_test.vars.positions[test_data.point2[1]],
      solver_test.vars.rotations[test_data.point1[1]],
      residual[1],
      pos_gradient1[1],
      pos_gradient2[1],
      rot_gradient[1]);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(pos_gradient1[0]), H.block(21, 3, 3, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-pos_gradient1[0], H.block(3, 21, 3, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(pos_gradient2[0]), H.block(21, 6, 3, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-pos_gradient2[0], H.block(6, 21, 3, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(rot_gradient[0]), H.block(21, 13, 3, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-rot_gradient[0], H.block(13, 21, 4, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(pos_gradient1[1]), H.block(24, 0, 3, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-pos_gradient1[1], H.block(0, 24, 3, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(pos_gradient2[1]), H.block(24, 3, 3, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-pos_gradient2[1], H.block(3, 24, 3, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(rot_gradient[1]), H.block(24, 9, 3, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-rot_gradient[1], H.block(9, 24, 4, 3), eps);

  /* Constraint lambda residuals. */
  const float3 target0 = target_residual(solver_test,
                                         residual[0],
                                         test_data.alphas[0],
                                         test_data.betas[0],
                                         test_data.lambdas[0]) +
                         target_velocity(solver_test,
                                         test_data.alphas[0],
                                         test_data.betas[0],
                                         test_data.point1[0],
                                         pos_gradient1[0]) +
                         target_velocity(solver_test,
                                         test_data.alphas[0],
                                         test_data.betas[0],
                                         test_data.point2[0],
                                         pos_gradient2[0]) +
                         target_angular_velocity(solver_test,
                                                 test_data.alphas[0],
                                                 test_data.betas[0],
                                                 test_data.point1[0],
                                                 rot_gradient[0]);
  const float3 target1 = target_residual(solver_test,
                                         residual[1],
                                         test_data.alphas[1],
                                         test_data.betas[1],
                                         test_data.lambdas[1]) +
                         target_velocity(solver_test,
                                         test_data.alphas[1],
                                         test_data.betas[1],
                                         test_data.point1[1],
                                         pos_gradient1[1]) +
                         target_velocity(solver_test,
                                         test_data.alphas[1],
                                         test_data.betas[1],
                                         test_data.point2[1],
                                         pos_gradient2[1]) +
                         target_angular_velocity(solver_test,
                                                 test_data.alphas[1],
                                                 test_data.betas[1],
                                                 test_data.point1[1],
                                                 rot_gradient[1]);
  EXPECT_NEAR(target0.x, b[21], eps);
  EXPECT_NEAR(target0.y, b[22], eps);
  EXPECT_NEAR(target0.z, b[23], eps);
  EXPECT_NEAR(target1.x, b[24], eps);
  EXPECT_NEAR(target1.y, b[25], eps);
  EXPECT_NEAR(target1.z, b[26], eps);

  EXPECT_EQ(system.constraint_mapping[0].size(), 2);
  EXPECT_EQ(system.constraint_mapping[0][0], 0);
  EXPECT_EQ(system.constraint_mapping[0][1], 1);
}

TEST_F(XPBDSolverTest, GlobalSolverConstraints_BendTwist)
{
  constexpr float eps = 1e-6f;

  SolverTestData solver_test = simple_solver_data({ConstraintType::BendTwist}, false);
  const auto &test_data = solver_test.bend_twist;

  IndexMaskMemory memory;
  xpbd_constraints::GlobalSolverSystem system = xpbd_constraints::build_global_solve_system(
      solver_test.params, solver_test.data, solver_test.vars, true, memory);
  const Eigen::SparseMatrix<float> &H = system.matrix;
  const Eigen::VectorXf &b = system.target;
  EXPECT_EQ(27, H.rows());
  EXPECT_EQ(27, H.cols());
  EXPECT_EQ(123, H.nonZeros());
  EXPECT_EQ(27, b.rows());
  EXPECT_EQ(system.constraint_mapping.size(), 1);

  EXPECT_EIGEN_V3_DIAG_NEAR(-compliance(solver_test, test_data.alphas[0], test_data.betas[0]),
                            H.block(21, 21, 3, 3),
                            eps);
  EXPECT_EIGEN_V3_DIAG_NEAR(-compliance(solver_test, test_data.alphas[1], test_data.betas[1]),
                            H.block(24, 24, 3, 3),
                            eps);

  float3 residual[2];
  float4x4 rot_gradient1[2], rot_gradient2[2];
  geometry::hair_constraints::eval_bend_twist_elements(
      test_data.darboux_vector[0],
      solver_test.vars.rotations[test_data.point1[0]],
      solver_test.vars.rotations[test_data.point2[0]],
      residual[0],
      rot_gradient1[0],
      rot_gradient2[0]);
  geometry::hair_constraints::eval_bend_twist_elements(
      test_data.darboux_vector[1],
      solver_test.vars.rotations[test_data.point1[1]],
      solver_test.vars.rotations[test_data.point2[1]],
      residual[1],
      rot_gradient1[1],
      rot_gradient2[1]);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(rot_gradient1[0]), H.block(21, 13, 3, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-rot_gradient1[0], H.block(13, 21, 4, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(rot_gradient2[0]), H.block(21, 17, 3, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-rot_gradient2[0], H.block(17, 21, 4, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(rot_gradient1[1]), H.block(24, 9, 3, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-rot_gradient1[1], H.block(9, 24, 4, 3), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-math::transpose(rot_gradient2[1]), H.block(24, 13, 3, 4), eps);
  EXPECT_EIGEN_MATRIX_NEAR(-rot_gradient2[1], H.block(13, 24, 4, 3), eps);

  /* Constraint lambda residuals. */
  const float3 target0 = target_residual(solver_test,
                                         residual[0],
                                         test_data.alphas[0],
                                         test_data.betas[0],
                                         test_data.lambdas[0]) +
                         target_angular_velocity(solver_test,
                                                 test_data.alphas[0],
                                                 test_data.betas[0],
                                                 test_data.point1[0],
                                                 rot_gradient1[0]) +
                         target_angular_velocity(solver_test,
                                                 test_data.alphas[0],
                                                 test_data.betas[0],
                                                 test_data.point2[0],
                                                 rot_gradient2[0]);
  const float3 target1 = target_residual(solver_test,
                                         residual[1],
                                         test_data.alphas[1],
                                         test_data.betas[1],
                                         test_data.lambdas[1]) +
                         target_angular_velocity(solver_test,
                                                 test_data.alphas[1],
                                                 test_data.betas[1],
                                                 test_data.point1[1],
                                                 rot_gradient1[1]) +
                         target_angular_velocity(solver_test,
                                                 test_data.alphas[1],
                                                 test_data.betas[1],
                                                 test_data.point2[1],
                                                 rot_gradient2[1]);
  EXPECT_NEAR(target0.x, b[21], eps);
  EXPECT_NEAR(target0.y, b[22], eps);
  EXPECT_NEAR(target0.z, b[23], eps);
  EXPECT_NEAR(target1.x, b[24], eps);
  EXPECT_NEAR(target1.y, b[25], eps);
  EXPECT_NEAR(target1.z, b[26], eps);

  EXPECT_EQ(system.constraint_mapping[0].size(), 2);
  EXPECT_EQ(system.constraint_mapping[0][0], 0);
  EXPECT_EQ(system.constraint_mapping[0][1], 1);
}

TEST_F(XPBDSolverTest, GlobalSolverConstraints_Contact)
{
  constexpr float eps = 1e-6f;

  SolverTestData solver_test = simple_solver_data({ConstraintType::Contact}, false);
  const auto &test_data = solver_test.contact;

  IndexMaskMemory memory;
  xpbd_constraints::GlobalSolverSystem system = xpbd_constraints::build_global_solve_system(
      solver_test.params, solver_test.data, solver_test.vars, true, memory);
  const Eigen::SparseMatrix<float> &H = system.matrix;
  const Eigen::VectorXf &b = system.target;
  EXPECT_EQ(23, H.rows());
  EXPECT_EQ(23, H.cols());
  EXPECT_EQ(51, H.nonZeros());
  EXPECT_EQ(23, b.rows());
  EXPECT_EQ(system.constraint_mapping.size(), 1);

  EXPECT_NEAR(-compliance(solver_test, 0.0f, 0.0f), H.coeff(21, 21), eps);
  EXPECT_NEAR(-compliance(solver_test, 0.0f, 0.0f), H.coeff(22, 22), eps);

  float residual[3];
  float3 pos_gradient1[3], pos_gradient_collider[3];
  float4 rot_gradient1[3], rot_gradient_collider[3];
  float3 collider_positions[2];
  math::Quaternion collider_rotations[2];
  float3 collider_scales[2];
  math::to_loc_rot_scale(solver_test.params.collider_transforms[0],
                         collider_positions[0],
                         collider_rotations[0],
                         collider_scales[0]);
  math::to_loc_rot_scale(solver_test.params.collider_transforms[1],
                         collider_positions[1],
                         collider_rotations[1],
                         collider_scales[1]);
  const bool active0 = geometry::hair_constraints::eval_contact_position_elements(
      test_data.local_position1[0],
      test_data.local_position2[0],
      test_data.normal[0],
      solver_test.vars.positions[test_data.point1[0]],
      collider_positions[test_data.collider_index[0]],
      solver_test.vars.rotations[test_data.point1[0]],
      collider_rotations[test_data.collider_index[0]],
      residual[0],
      pos_gradient1[0],
      pos_gradient_collider[0],
      rot_gradient1[0],
      rot_gradient_collider[0]);
  const bool active1 = geometry::hair_constraints::eval_contact_position_elements(
      test_data.local_position1[1],
      test_data.local_position2[1],
      test_data.normal[1],
      solver_test.vars.positions[test_data.point1[1]],
      collider_positions[test_data.collider_index[1]],
      solver_test.vars.rotations[test_data.point1[1]],
      collider_rotations[test_data.collider_index[1]],
      residual[1],
      pos_gradient1[1],
      pos_gradient_collider[1],
      rot_gradient1[1],
      rot_gradient_collider[1]);
  const bool active2 = geometry::hair_constraints::eval_contact_position_elements(
      test_data.local_position1[2],
      test_data.local_position2[2],
      test_data.normal[2],
      solver_test.vars.positions[test_data.point1[2]],
      collider_positions[test_data.collider_index[2]],
      solver_test.vars.rotations[test_data.point1[2]],
      collider_rotations[test_data.collider_index[2]],
      residual[2],
      pos_gradient1[2],
      pos_gradient_collider[2],
      rot_gradient1[2],
      rot_gradient_collider[2]);
  EXPECT_TRUE(active0);
  EXPECT_FALSE(active1);
  EXPECT_TRUE(active2);
  /* Note: no entries for contact [1] because it is inactive. */
  EXPECT_EIGEN_V3_ROW_NEAR(-pos_gradient1[0], H.block(21, 3, 1, 3), eps);
  EXPECT_EIGEN_V3_COL_NEAR(-pos_gradient1[0], H.block(3, 21, 3, 1), eps);
  EXPECT_EIGEN_V4_ROW_NEAR(-rot_gradient1[0], H.block(21, 13, 1, 4), eps);
  EXPECT_EIGEN_V4_COL_NEAR(-rot_gradient1[0], H.block(13, 21, 4, 1), eps);
  EXPECT_EIGEN_V3_ROW_NEAR(-pos_gradient1[2], H.block(22, 0, 1, 3), eps);
  EXPECT_EIGEN_V3_COL_NEAR(-pos_gradient1[2], H.block(0, 22, 3, 1), eps);
  EXPECT_EIGEN_V4_ROW_NEAR(-rot_gradient1[2], H.block(22, 9, 1, 4), eps);
  EXPECT_EIGEN_V4_COL_NEAR(-rot_gradient1[2], H.block(9, 22, 4, 1), eps);

  /* Constraint lambda residuals. */
  const float target0 = target_residual(solver_test,
                                        residual[0],
                                        test_data.alphas[0],
                                        test_data.betas[0],
                                        test_data.lambdas[0]) +
                        target_velocity(solver_test,
                                        test_data.alphas[0],
                                        test_data.betas[0],
                                        test_data.point1[0],
                                        pos_gradient1[0]) +
                        target_angular_velocity(solver_test,
                                                test_data.alphas[0],
                                                test_data.betas[0],
                                                test_data.point1[0],
                                                rot_gradient1[0]);
  const float target2 = target_residual(solver_test,
                                        residual[2],
                                        test_data.alphas[2],
                                        test_data.betas[2],
                                        test_data.lambdas[2]) +
                        target_velocity(solver_test,
                                        test_data.alphas[2],
                                        test_data.betas[2],
                                        test_data.point1[2],
                                        pos_gradient1[2]) +
                        target_angular_velocity(solver_test,
                                                test_data.alphas[2],
                                                test_data.betas[2],
                                                test_data.point1[2],
                                                rot_gradient1[2]);
  EXPECT_NEAR(target0, b[21], eps);
  EXPECT_NEAR(target2, b[22], eps);

  EXPECT_EQ(system.constraint_mapping[0].size(), 2);
  EXPECT_EQ(system.constraint_mapping[0][0], 0);
  EXPECT_EQ(system.constraint_mapping[0][1], 2);
}

TEST_F(XPBDSolverTest, GlobalSolverExecute)
{
  // constexpr float eps = 1e-6f;

  SolverTestData solver_test = simple_solver_data(
      {ConstraintType::PositionGoal, ConstraintType::BendTwist, ConstraintType::Contact}, false);

  IndexMaskMemory memory;
  xpbd_constraints::GlobalSolverSystem system = xpbd_constraints::build_global_solve_system(
      solver_test.params, solver_test.data, solver_test.vars, true, memory);
  /* Print matrix for debugging purposes if necessary. */
  if (false) {
    const Eigen::IOFormat format;
    std::cout << system.matrix.toDense().format(format) << std::endl;
    std::cout << system.target.format(format) << std::endl;
  }

  xpbd_constraints::SolverResult result = xpbd_constraints::solve_global_system(
      std::move(system), solver_test.vars, solver_test.data);
  EXPECT_EQ(result, xpbd_constraints::SolverResult::Success);
}

}  // namespace blender::nodes::tests
