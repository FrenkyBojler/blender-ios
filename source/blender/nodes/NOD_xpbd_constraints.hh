/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <atomic>

#include "BLI_function_ref.hh"
#include "BLI_math_axis_angle.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_string_ref.hh"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"

#include "NOD_geometry_nodes_bundle.hh"

namespace blender::nodes::xpbd_constraints {

struct ConstraintTypeInfo;
struct ConstraintVariables;
struct DebugRecorder;

/* -------------------------------------------------------------------- */
/** \name Solver Parameters
 * \{ */

struct ConstraintEvalParams {
  using ErrorFn = std::function<void(const StringRef message)>;

  ErrorFn error_message_add;
  /* Perform debug checks on user inputs at runtime. This helps avoid common errors but has a
   * significant performance cost, so should be optional. */
  bool debug_check;
  std::unique_ptr<DebugRecorder> debug_recorder;

  float delta_time;
  float delta_time_squared;
  float inv_delta_time;
  float inv_delta_time_squared;

  /** Positions before applying constraints. */
  VArraySpan<float3> old_positions;
  /** Rotations before applying constraints. */
  VArraySpan<math::Quaternion> old_rotations;

  /** Velocities derived after positional constraints. */
  Span<float3> orig_velocities;
  /** Angular velocities derived after positional constraints. */
  Span<float3> orig_angular_velocities;

  /* Linear point masses. */
  VArraySpan<float> masses;
  /* Moments of inertia in the local frame. */
  VArraySpan<float3> local_inertia;

  /** Collider transform at the end of the current time. */
  Span<float4x4> collider_transforms;
  /** Collider transforms at the end of the previous frame. */
  Span<float4x4> old_collider_transforms;
};

struct ConstraintVariables {
  /** Positions after applying constraints. */
  Array<float3> positions;
  /** Rotations after applying constraints. */
  Array<math::Quaternion> rotations;
  /** Velocities after applying constraints. */
  Array<float3> velocities;
  /** Angular velocities after applying constraints. */
  Array<float3> angular_velocities;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Constraint Functions
 * \{ */

/**
 * Returns the number of components used by a constraint.
 * \param r_num_components Number of components, or Lagrange multipliers (lambda), used by a single
 * constraint, typically up to 3.
 * \param r_num_position_vars Number of position variables used by a single constraint, up to 4.
 * \param r_num_rotation_vars Number of rotation variables used by a single constraint, up to 4.
 */
using ConstraintSizeFunc = std::function<void(int &r_num_components,
                                              int &r_num_position_vars,
                                              int &r_num_rotation_vars,
                                              bool &r_use_active_mask)>;

/**
 * Returns the variable indices used by a constraint.
 */
using ConstraintVariableIndicesFunc = std::function<void(const bke::AttributeAccessor &attributes,
                                                         const IndexMask &selection,
                                                         MutableSpan<int> r_position_indices[4],
                                                         MutableSpan<int> r_rotation_indices[4])>;

using ConstraintInitStepFunc = std::function<void(bke::GeometrySet &constraints)>;

/**
 * Evaluates position constraints based on current geometry state.
 * It should write the results to attributes in the constraints geometry, which are then
 * applied to the geometry by the solver using the constraint mapping.
 */
using ConstraintEvalPositionFunc = std::function<void(const ConstraintEvalParams &eval_params,
                                                      const ConstraintVariables &variables,
                                                      const IndexMask &group_mask,
                                                      bke::GeometrySet &constraints,
                                                      VArray<bool> &r_active,
                                                      Vector<VArray<float3>> &r_delta_positions,
                                                      Vector<VArray<float4>> &r_delta_rotations)>;
/**
 * Evaluates velocity constraints based on current geometry state.
 * It should write the results to attributes in the constraints geometry, which are then applied to
 * the geometry by the solver using the constraint mapping.
 */
using ConstraintEvalVelocityFunc =
    std::function<void(const ConstraintEvalParams &eval_params,
                       const ConstraintVariables &variables,
                       const IndexMask &group_mask,
                       bke::GeometrySet &constraints,
                       VArray<bool> &r_active,
                       Vector<VArray<float3>> &r_delta_velocities,
                       Vector<VArray<float3>> &r_delta_angular_velocities)>;

/**
 * Compute elements of the constraint matrix for a global linear constraint solve.
 * The number of components and affected variables is defined by the separate size function.
 * This also determines the data type of the fields expected from this function (float, float2,
 * float3).
 *
 * Spans are compressed and contain only values for constraints in the index mask.
 * They must be addressed by the position in the mask, not the index of the constraint.
 *
 * Gradients for positions and rotations expand the data type to 3 or 4 rows respectively
 * (transposed Jacobian derivative matrix):
 *
 * | Residual Type | Position Gradient | Rotation Gradient |
 * |   float       |   float3          |   float4          |
 * |   float2      |   float2x3        |   float2x4        |
 * |   float3      |   float3x3        |   float4x4        |
 *
 * The function receives up to 4 spans for gradients depending on the number of variables it uses,
 * as defined by the size function
 *
 * The solver constructs a linear system that yields constraint impulses and variable offsets.
 * For a detailed derivation see for example:
 *   Kugelstadt, "Direct Position-Based Solver for Stiff Rods", 2018
 *   Soler, "Cosserat Rods with Projective Dynamics", 2018
 *
 * \param params General parameters of the current evaluation.
 * \param variables Current state of the simulated geometry.
 * \param attributes Attributes of the constraint data.
 * \param selection Selection of constraints evaluated by the solver.
 * \param r_alphas Compliance values (softness).
 * \param r_betas Damping values.
 * \param r_residuals Residual values in the current configuration.
 * \param r_position_gradients Gradients for affected position variables.
 * \param r_rotation_gradients Gradients for affected rotation variables.
 * \param r_position_indices Position variable indices.
 * \param r_rotation_indices Rotation variable indices.
 */
using ConstraintPositionLinearSolveElementsFunc =
    std::function<void(const ConstraintEvalParams &params,
                       const ConstraintVariables &variables,
                       const bke::AttributeAccessor &attributes,
                       const IndexMask &selection,
                       GMutableSpan r_alphas,
                       GMutableSpan r_betas,
                       GMutableSpan r_residuals,
                       GMutableSpan r_position_gradients[4],
                       GMutableSpan r_rotation_gradients[4],
                       MutableSpan<bool> r_active_mask)>;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Constraint Types
 * \{ */

struct ConstraintBundleItems {
  static const SocketInterfaceKey stretch_constraints_key;
  static const SocketInterfaceKey bending_constraints_key;
  static const SocketInterfaceKey position_constraints_key;
  static const SocketInterfaceKey rotation_constraints_key;
  static const SocketInterfaceKey contact_constraints_key;

  bke::GeometrySet stretch_constraints;
  bke::GeometrySet bending_constraints;
  bke::GeometrySet position_constraints;
  bke::GeometrySet rotation_constraints;
  bke::GeometrySet contact_constraints;
};

void set_constraints(BundlePtr &bundle_ptr,
                     const SocketInterfaceKey &key,
                     const bke::GeometrySet &geometry);
bke::GeometrySet lookup_constraints(const Bundle &bundle, const SocketInterfaceKey &key);
BundlePtr combine_constraint_bundle(const ConstraintBundleItems &items);
void separate_constraint_bundle(const Bundle &bundle, ConstraintBundleItems &items);

struct ConstraintTypeInfo {
  using ErrorFn = ConstraintEvalParams::ErrorFn;

  std::string ui_name;
  std::string ui_description;
  int type_code;

  ConstraintSizeFunc get_size;
  ConstraintVariableIndicesFunc get_variable_indices;

  ConstraintInitStepFunc init_step;
  ConstraintEvalPositionFunc evaluate_position;
  ConstraintEvalVelocityFunc evaluate_velocity;
  ConstraintPositionLinearSolveElementsFunc linear_solve_elements;
};

const ConstraintTypeInfo &get_info(const SocketInterfaceKey &key, bool debug_check);

Span<ConstraintTypeInfo> get_constraint_info(bool debug_output);
Span<ConstraintTypeInfo> get_constraint_info_ordered(bool debug_output);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Low-level Constraint Functions
 * \{ */

/** Add a positive or negative offset depending on the quaternion sign. */
inline math::Quaternion quaternion_offset(const math::Quaternion &q, const float4 &offset)
{
  // return q.w >= 0 ? math::Quaternion(float4(q) + offset) : math::Quaternion(float4(q) - offset);
  return math::Quaternion(float4(q) + offset);
}

inline void apply_position_impulse(const float3 &delta_position, float3 &position)
{
  position += delta_position;
}

template<bool linearized_quaternion>
inline void apply_rotation_impulse(const float4 &delta_rotation, math::Quaternion &rotation)
{
  if constexpr (linearized_quaternion) {
    rotation = math::normalize(quaternion_offset(rotation, delta_rotation));
  }
  else {
    /* Normalize the final result to avoid accumulating errors. */
    constexpr bool normalize_final = true;

    rotation = math::Quaternion(delta_rotation) * rotation;
    if constexpr (normalize_final) {
      rotation = math::normalize(rotation);
    }
  }
}

inline void apply_velocity_impulse(const float3 &delta_velocity, float3 &velocity)
{
  velocity += delta_velocity;
}

inline void apply_angular_velocity_impulse(const float3 &delta_angular_velocity,
                                           float3 &angular_velocity)
{
  angular_velocity += delta_angular_velocity;
}

inline void eval_position_goal(const float3 &goal_position,
                               const float alpha,
                               const float gamma,
                               const float lambda,
                               const float3 &position,
                               const float3 &old_position,
                               float &r_residual,
                               float &r_delta_lambda,
                               float3 &r_delta_position)
{
  const float weight_norm = math::safe_rcp(1.0f + gamma + alpha);

  const float3 gradient = math::normalize_and_get_length(position - goal_position, r_residual);
  const float velocity = math::dot(gradient, position - old_position);

  r_delta_lambda = weight_norm * (-r_residual - alpha * lambda - gamma * velocity);
  r_delta_position = r_delta_lambda * gradient;
}

inline void eval_position_goal_elements(const float3 &goal_position,
                                        const float3 &position,
                                        float &r_residual,
                                        float3 &r_gradient)
{
  r_gradient = math::normalize_and_get_length(position - goal_position, r_residual);
}

inline void apply_position_goal(const float3 &goal_position,
                                const float alpha,
                                float &lambda,
                                float3 &position)
{
  float residual;
  float delta_lambda;
  float3 delta_position;
  eval_position_goal(goal_position,
                     alpha,
                     0.0f,
                     lambda,
                     position,
                     float3(0.0f),
                     residual,
                     delta_lambda,
                     delta_position);

  lambda += delta_lambda;
  apply_position_impulse(delta_position, position);
}

inline void apply_position_goal(const float3 &goal_position,
                                const float alpha,
                                const float gamma,
                                const float3 &old_position,
                                float &lambda,
                                float3 &position)
{
  float residual;
  float delta_lambda;
  float3 delta_position;
  eval_position_goal(goal_position,
                     alpha,
                     gamma,
                     lambda,
                     position,
                     old_position,
                     residual,
                     delta_lambda,
                     delta_position);

  lambda += delta_lambda;
  apply_position_impulse(delta_position, position);
}

template<bool linearized_quaternion>
inline void eval_rotation_goal(const math::Quaternion &goal_rotation,
                               const float alpha,
                               const float gamma,
                               const float lambda,
                               const math::Quaternion &rotation,
                               const math::Quaternion &old_rotation,
                               float &r_residual,
                               float &r_delta_lambda,
                               float4 &r_delta_rotation)
{
  const float weight_norm = math::safe_rcp(1.0f + gamma + alpha);

  const math::AxisAngle axis_angle = math::to_axis_angle(rotation *
                                                         math::invert_normalized(goal_rotation));
  r_residual = axis_angle.angle().wrapped().radian();
  const float3 gradient = axis_angle.axis();
  const math::AxisAngle rotation_diff = math::to_axis_angle(math::invert_normalized(old_rotation) *
                                                            rotation);
  const float velocity = rotation_diff.angle().wrapped().radian();

  r_delta_lambda = weight_norm * (-r_residual - alpha * lambda - gamma * velocity);

  if constexpr (linearized_quaternion) {
    // const float4 q = float4(-math::dot(gradient, rotation.imaginary_part()),
    //                               rotation.w * gradient +
    //                                   math::cross(gradient, rotation.imaginary_part()));
    const float4 q = float4(math::Quaternion(0.0f, gradient) * rotation);
    r_delta_rotation = r_delta_lambda * 0.5f * q;
  }
  else {
    r_delta_rotation = float4(
        math::to_quaternion(math::AxisAngle(gradient, math::AngleRadian(r_delta_lambda))));
  }
}

template<bool linearized_quaternion>
inline void apply_rotation_goal(const math::Quaternion &goal_rotation,
                                const float alpha,
                                float &lambda,
                                math::Quaternion &rotation)
{
  float residual;
  float delta_lambda;
  float4 delta_rotation;
  eval_rotation_goal<linearized_quaternion>(goal_rotation,
                                            alpha,
                                            0.0f,
                                            lambda,
                                            rotation,
                                            math::Quaternion::identity(),
                                            residual,
                                            delta_lambda,
                                            delta_rotation);

  lambda += delta_lambda;
  apply_rotation_impulse<linearized_quaternion>(delta_rotation, rotation);
}

template<bool linearized_quaternion>
inline void apply_rotation_goal(const math::Quaternion &goal_rotation,
                                const float alpha,
                                const float gamma,
                                const math::Quaternion &old_rotation,
                                float &lambda,
                                math::Quaternion &rotation)
{
  float residual;
  float delta_lambda;
  float4 delta_rotation;
  eval_rotation_goal<linearized_quaternion>(goal_rotation,
                                            alpha,
                                            gamma,
                                            lambda,
                                            rotation,
                                            old_rotation,
                                            residual,
                                            delta_lambda,
                                            delta_rotation);

  lambda += delta_lambda;
  apply_rotation_impulse<linearized_quaternion>(delta_rotation, rotation);
}

inline void eval_velocity_goal(const float3 &goal_velocity,
                               const float beta,
                               const float lambda,
                               const float3 &velocity,
                               float &r_residual,
                               float &r_delta_lambda,
                               float3 &r_delta_velocity)
{
  const float weight_norm = math::safe_rcp(1.0f + beta);

  const float3 gradient = math::normalize_and_get_length(velocity - goal_velocity, r_residual);

  r_delta_lambda = weight_norm * (-beta * r_residual - lambda);
  r_delta_velocity = r_delta_lambda * gradient;
}

inline void apply_velocity_goal(const float3 &goal_velocity,
                                const float beta,
                                float &lambda,
                                float3 &velocity)
{
  float residual;
  float delta_lambda;
  float3 delta_velocity;
  eval_velocity_goal(
      goal_velocity, beta, lambda, velocity, residual, delta_lambda, delta_velocity);

  lambda += delta_lambda;
  apply_velocity_impulse(delta_velocity, velocity);
}

inline void eval_angular_velocity_goal(const float3 &goal_angular_velocity,
                                       const float beta,
                                       const float lambda,
                                       const float3 &angular_velocity,
                                       float &r_residual,
                                       float &r_delta_lambda,
                                       float3 &r_delta_angular_velocity)
{
  const float weight_norm = math::safe_rcp(1.0f + beta);

  const float3 gradient = math::normalize_and_get_length(angular_velocity - goal_angular_velocity,
                                                         r_residual);

  r_delta_lambda = weight_norm * (-beta * r_residual - lambda);
  r_delta_angular_velocity = r_delta_lambda * gradient;
}

inline void apply_angular_velocity_goal(const float3 &goal_angular_velocity,
                                        const float beta,
                                        float &lambda,
                                        float3 &angular_velocity)
{
  float residual;
  float delta_lambda;
  float3 delta_angular_velocity;
  eval_angular_velocity_goal(goal_angular_velocity,
                             beta,
                             lambda,
                             angular_velocity,
                             residual,
                             delta_lambda,
                             delta_angular_velocity);

  lambda += delta_lambda;
  apply_angular_velocity_impulse(delta_angular_velocity, angular_velocity);
}

template<bool linearized_quaternion>
inline void eval_position_stretch_shear(const float weight_pos1,
                                        const float weight_pos2,
                                        const float weight_rot,
                                        const float edge_length,
                                        const float alpha,
                                        const float gamma,
                                        const float3 &lambda,
                                        const float3 &position1,
                                        const float3 &position2,
                                        const math::Quaternion &rotation,
                                        const float3 &old_position1,
                                        const float3 &old_position2,
                                        const math::Quaternion &old_rotation,
                                        float3 &r_residual,
                                        float3 &r_delta_lambda,
                                        float3 &r_delta_position1,
                                        float3 &r_delta_position2,
                                        float4 &r_delta_rotation)
{
  const float inv_edge_length = math::safe_rcp(edge_length);
  const float weight_norm = math::safe_rcp(
      ((weight_pos1 + weight_pos2) * inv_edge_length * inv_edge_length + 4.0f * weight_rot) *
          (1.0f + gamma) +
      alpha);

  const float3 direction = math::transform_point(rotation, float3(0, 0, 1));
  r_residual = inv_edge_length * (position2 - position1) - direction;

  /* Constraint gradient applied to variable differences.
   * This extends the rod constraints from "Position and Orientation Based Cosserat Rods"
   * (Kugelstadt et al.), section 6, with the damping terms for lambda from the XPBD paper ("XPBD:
   * Position-Based Simulation of Compliant Constrained Dynamics", Macklin et al.).
   */
  const math::Quaternion q3 = {0, 0, 0, 1};
  const float3 lambda_damping =
      ((position1 - old_position1) - (position2 - old_position2)) * inv_edge_length +
      2.0f * (direction - (math::conjugate(old_rotation) * q3 * rotation).imaginary_part());

  r_delta_lambda = weight_norm * (r_residual - alpha * lambda - gamma * lambda_damping);

  r_delta_position1 = weight_pos1 * inv_edge_length * r_delta_lambda;
  r_delta_position2 = -weight_pos2 * inv_edge_length * r_delta_lambda;
  if constexpr (linearized_quaternion) {
    r_delta_rotation = weight_rot * float4(math::Quaternion(0.0f, r_delta_lambda) * rotation *
                                           math::Quaternion(0, 0, 0, -1));
  }
  else {
    r_delta_rotation = float4(
        math::to_quaternion(math::AxisAngle(direction, math::normalize(position2 - position1))));
  }
}

inline void eval_stretch_shear_elements(const float edge_length,
                                        const float3 &position1,
                                        const float3 &position2,
                                        const math::Quaternion &rotation,
                                        float3 &r_residual,
                                        float4x4 &r_pos_gradient1,
                                        float4x4 &r_pos_gradient2,
                                        float4x4 &r_rot_gradient)
{
  const float inv_edge_length = math::safe_rcp(edge_length);

  const float3 direction = math::transform_point(rotation, float3(0, 0, 1));
  r_residual = inv_edge_length * (position2 - position1) - direction;

  r_pos_gradient1 = -inv_edge_length * float4x4::identity();
  r_pos_gradient2 = -r_pos_gradient1;

  const math::Quaternion q = math::Quaternion(0, 0, 0, 1.0f) * math::invert_normalized(rotation);
  r_rot_gradient = float4x4::zero();
  r_rot_gradient[0] = -2.0f * float4(q.x, q.w, q.z, -q.y);
  r_rot_gradient[1] = -2.0f * float4(q.y, -q.z, q.w, q.x);
  r_rot_gradient[2] = -2.0f * float4(q.z, q.y, -q.x, q.w);
  /* Last column is unused. */
}

template<bool linearized_quaternion>
inline void apply_position_stretch_shear(const float weight_pos1,
                                         const float weight_pos2,
                                         const float weight_rot,
                                         const float edge_length,
                                         const float alpha,
                                         float3 &lambda,
                                         float3 &position1,
                                         float3 &position2,
                                         math::Quaternion &rotation)
{
  float3 residual;
  float3 delta_lambda;
  float3 delta_pos1, delta_pos2;
  float4 delta_rot;
  eval_position_stretch_shear<linearized_quaternion>(weight_pos1,
                                                     weight_pos2,
                                                     weight_rot,
                                                     edge_length,
                                                     alpha,
                                                     0.0f,
                                                     lambda,
                                                     position1,
                                                     position2,
                                                     rotation,
                                                     float3(0.0f),
                                                     float3(0.0f),
                                                     math::Quaternion::identity(),
                                                     residual,
                                                     delta_lambda,
                                                     delta_pos1,
                                                     delta_pos2,
                                                     delta_rot);

  lambda += delta_lambda;
  apply_position_impulse(delta_pos1, position1);
  apply_position_impulse(delta_pos2, position2);
  apply_rotation_impulse<linearized_quaternion>(delta_rot, rotation);
}

template<bool linearized_quaternion>
inline void apply_position_stretch_shear(const float weight_pos1,
                                         const float weight_pos2,
                                         const float weight_rot,
                                         const float edge_length,
                                         const float alpha,
                                         const float gamma,
                                         const float3 &old_position1,
                                         const float3 &old_position2,
                                         const math::Quaternion &old_rotation,
                                         float3 &lambda,
                                         float3 &position1,
                                         float3 &position2,
                                         math::Quaternion &rotation)
{
  float3 residual;
  float3 delta_lambda;
  float3 delta_pos1, delta_pos2;
  float4 delta_rot;
  eval_position_stretch_shear<linearized_quaternion>(weight_pos1,
                                                     weight_pos2,
                                                     weight_rot,
                                                     edge_length,
                                                     alpha,
                                                     gamma,
                                                     lambda,
                                                     position1,
                                                     position2,
                                                     rotation,
                                                     old_position1,
                                                     old_position2,
                                                     old_rotation,
                                                     residual,
                                                     delta_lambda,
                                                     delta_pos1,
                                                     delta_pos2,
                                                     delta_rot);

  lambda += delta_lambda;
  apply_position_impulse(delta_pos1, position1);
  apply_position_impulse(delta_pos2, position2);
  apply_rotation_impulse<linearized_quaternion>(delta_rot, rotation);
}

template<bool linearized_quaternion>
inline void eval_position_bend_twist(const float3 &weight_rot1,
                                     const float3 &weight_rot2,
                                     const float3 &darboux_vector,
                                     const float3 &alpha,
                                     const float3 &gamma,
                                     const float3 &lambda,
                                     const math::Quaternion &rotation1,
                                     const math::Quaternion &rotation2,
                                     const math::Quaternion &old_rotation1,
                                     const math::Quaternion &old_rotation2,
                                     float3 &r_residual,
                                     float3 &r_delta_lambda,
                                     float4 &r_delta_rotation1,
                                     float4 &r_delta_rotation2)
{
  const float3 weight_norm = math::safe_rcp((weight_rot1 + weight_rot2) * (float3(1.0f) + gamma) +
                                            alpha);

  const float3 current_darboux = (math::invert_normalized(rotation1) * rotation2).imaginary_part();
  r_residual = current_darboux - darboux_vector;

  /* Constraint gradient applied to variable differences.
   * This extends the rod constraints from "Position and Orientation Based Cosserat Rods"
   * (Kugelstadt et al.), section 6, with the damping terms for lambda from the XPBD paper
   * ("XPBD: Position-Based Simulation of Compliant Constrained Dynamics", Macklin et al.). */
  const float3 velocity = (math::conjugate(rotation1) * rotation2).imaginary_part() -
                          (math::conjugate(old_rotation1) * old_rotation2).imaginary_part();

  r_delta_lambda = weight_norm * (-r_residual - alpha * lambda - gamma * velocity);

  if constexpr (linearized_quaternion) {
    r_delta_rotation1 = float4(
        rotation2 * math::conjugate(math::Quaternion(0.0f, r_delta_lambda * weight_rot1)));
    r_delta_rotation2 = float4(rotation1 * math::Quaternion(0.0f, r_delta_lambda * weight_rot2));
  }
  else {
    // TODO
    BLI_assert_unreachable();
  }
}

inline void eval_bend_twist_elements(const float3 &darboux_vector,
                                     const math::Quaternion &rotation1,
                                     const math::Quaternion &rotation2,
                                     float3 &r_residual,
                                     float4x4 &r_gradient1,
                                     float4x4 &r_gradient2)
{
  const float3 current_darboux = (math::invert_normalized(rotation1) * rotation2).imaginary_part();
  r_residual = current_darboux - darboux_vector;

  r_gradient1[0] = float4(rotation2.x, -rotation2.w, -rotation2.z, rotation2.y);
  r_gradient1[1] = float4(rotation2.y, rotation2.z, -rotation2.w, -rotation2.x);
  r_gradient1[2] = float4(rotation2.z, -rotation2.y, rotation2.x, -rotation2.w);
  /* Last column is unused. */
  r_gradient2[0] = float4(-rotation1.x, rotation1.w, rotation1.z, -rotation1.y);
  r_gradient2[1] = float4(-rotation1.y, -rotation1.z, rotation1.w, rotation1.x);
  r_gradient2[2] = float4(-rotation1.z, rotation1.y, -rotation1.x, rotation1.w);
  /* Last column is unused. */
}

template<bool linearized_quaternion>
inline void apply_position_bend_twist(const float3 &weight_rot1,
                                      const float3 &weight_rot2,
                                      const float3 &darboux_vector,
                                      const float3 &alpha,
                                      float3 &lambda,
                                      math::Quaternion &rotation1,
                                      math::Quaternion &rotation2)
{
  float3 residual;
  float3 delta_lambda;
  float4 delta_rot1, delta_rot2;
  eval_position_bend_twist<linearized_quaternion>(weight_rot1,
                                                  weight_rot2,
                                                  darboux_vector,
                                                  alpha,
                                                  float3(0.0f),
                                                  lambda,
                                                  rotation1,
                                                  rotation2,
                                                  math::Quaternion::identity(),
                                                  math::Quaternion::identity(),
                                                  residual,
                                                  delta_lambda,
                                                  delta_rot1,
                                                  delta_rot2);

  lambda += delta_lambda;
  apply_rotation_impulse<linearized_quaternion>(delta_rot1, rotation1);
  apply_rotation_impulse<linearized_quaternion>(delta_rot2, rotation2);
}

template<bool linearized_quaternion>
inline void apply_position_bend_twist(const float weight_rot1,
                                      const float weight_rot2,
                                      const float3 &darboux_vector,
                                      const float3 &alpha,
                                      const float3 &gamma,
                                      const math::Quaternion &old_rotation1,
                                      const math::Quaternion &old_rotation2,
                                      float3 &lambda,
                                      math::Quaternion &rotation1,
                                      math::Quaternion &rotation2)
{
  float3 residual;
  float3 delta_lambda;
  float4 delta_rot1, delta_rot2;
  eval_position_bend_twist<linearized_quaternion>(weight_rot1,
                                                  weight_rot2,
                                                  darboux_vector,
                                                  alpha,
                                                  gamma,
                                                  lambda,
                                                  rotation1,
                                                  rotation2,
                                                  old_rotation1,
                                                  old_rotation2,
                                                  residual,
                                                  delta_lambda,
                                                  delta_rot1,
                                                  delta_rot2);

  lambda += delta_lambda;
  apply_rotation_impulse<linearized_quaternion>(delta_rot1, rotation1);
  apply_rotation_impulse<linearized_quaternion>(delta_rot2, rotation2);
}

template<bool linearized_quaternion>
inline void eval_rotation_goal2(const float3 &weight_rot,
                                const math::Quaternion &goal_rotation,
                                const float3 &alpha,
                                const float3 &gamma,
                                const float3 &lambda,
                                const math::Quaternion &rotation,
                                const math::Quaternion &old_rotation,
                                float3 &r_residual,
                                float3 &r_delta_lambda,
                                float4 &r_delta_rotation)
{
  const float3 darboux_vector = float3(0.0f);
  /* TODO account for root animation. */
  const math::Quaternion old_goal_rotation = goal_rotation;
  float4 delta_root_rotation;
  eval_position_bend_twist<linearized_quaternion>(float3(0.0f),
                                                  weight_rot,
                                                  darboux_vector,
                                                  alpha,
                                                  gamma,
                                                  lambda,
                                                  goal_rotation,
                                                  rotation,
                                                  old_goal_rotation,
                                                  old_rotation,
                                                  r_residual,
                                                  r_delta_lambda,
                                                  delta_root_rotation,
                                                  r_delta_rotation);
}

inline void eval_rotation_goal_elements(const math::Quaternion &goal_rotation,
                                        const math::Quaternion &rotation,
                                        float3 &r_residual,
                                        float4x4 &r_gradient)
{
  const float3 darboux_vector = float3(0.0f);
  float4x4 root_gradient;
  eval_bend_twist_elements(
      darboux_vector, goal_rotation, rotation, r_residual, root_gradient, r_gradient);
}

template<bool linearized_quaternion>
inline void apply_rotation_goal2(const math::Quaternion &goal_rotation,
                                 const float3 &alpha,
                                 float4 &lambda,
                                 math::Quaternion &rotation)
{
  float residual;
  float4 delta_lambda;
  float4 delta_rotation;
  eval_rotation_goal2<linearized_quaternion>(goal_rotation,
                                             alpha,
                                             float3(0.0f),
                                             lambda,
                                             rotation,
                                             math::Quaternion::identity(),
                                             residual,
                                             delta_lambda,
                                             delta_rotation);

  lambda += delta_lambda;
  apply_rotation_impulse<linearized_quaternion>(delta_rotation, rotation);
}

template<bool linearized_quaternion>
inline void apply_rotation_goal2(const math::Quaternion &goal_rotation,
                                 const float3 &alpha,
                                 const float3 &gamma,
                                 const math::Quaternion &old_rotation,
                                 float3 &lambda,
                                 math::Quaternion &rotation)
{
  float3 residual;
  float3 delta_lambda;
  float4 delta_rotation;
  eval_rotation_goal2<linearized_quaternion>(goal_rotation,
                                             alpha,
                                             gamma,
                                             lambda,
                                             rotation,
                                             old_rotation,
                                             residual,
                                             delta_lambda,
                                             delta_rotation);

  lambda += delta_lambda;
  apply_rotation_impulse<linearized_quaternion>(delta_rotation, rotation);
}

/**
 * Positional contact constraint based on
 * "Detailed Rigid Body Simulation with Extended Position Based Dynamics", Mueller et al., 2020
 */
inline bool eval_position_contact(const float weight_pos1,
                                  const float weight_pos2,
                                  const float weight_rot1,
                                  const float weight_rot2,
                                  const float3 &local_position1,
                                  const float3 &local_position2,
                                  const float3 &normal,
                                  const float alpha,
                                  const float lambda,
                                  const float3 &position1,
                                  const float3 &position2,
                                  const math::Quaternion &rotation1,
                                  const math::Quaternion &rotation2,
                                  float &r_residual_depth,
                                  float &r_delta_lambda,
                                  float3 &r_delta_position1,
                                  float3 &r_delta_position2,
                                  float4 &r_delta_rotation1,
                                  float4 &r_delta_rotation2)
{
  /* Local positions are relative to colliders.
   * Normal is a fixed shared direction for both participants. */

  /* Contact points are computed by applying the transforms to relative local positions. */
  const float3 contact_point1 = math::transform_point(rotation1, local_position1) + position1;
  const float3 contact_point2 = math::transform_point(rotation2, local_position2) + position2;

  /* Positional constraint for penetration depth along the normal. */
  r_residual_depth = math::dot(contact_point1 - contact_point2, normal);
  /* Only act on contact. */
  const bool active = r_residual_depth < 0.0f;
  if (!active) {
    return false;
  }

  /* Effective mass correction to account for the effect of rotation on position displacement.
   * See section 3.3.1 "Positional Constraints" of the paper.
   * We use a simplified rotational weight factor instead of full inverse moment of inertia. */
  const float rot_factor1 = math::length_squared(local_position1) -
                            math::square(math::dot(local_position1, normal));
  const float rot_factor2 = math::length_squared(local_position2) -
                            math::square(math::dot(local_position2, normal));
  const float weight_norm = math::safe_rcp(weight_pos1 + weight_rot1 * rot_factor1 + weight_pos2 +
                                           weight_rot2 * rot_factor2 + alpha);

  /* Gradient is normal, length is 1, no need to compute gradient norm. */
  r_delta_lambda = weight_norm * (-r_residual_depth - alpha * lambda);
  const float3 impulse1 = r_delta_lambda * normal;
  const float3 impulse2 = -impulse1;

  r_delta_position1 = impulse1 * weight_pos1;
  r_delta_position2 = impulse2 * weight_pos2;
  r_delta_rotation1 = float4(0.0f, math::cross(local_position1, impulse1)) * weight_rot1;
  r_delta_rotation2 = float4(0.0f, math::cross(local_position2, impulse2)) * weight_rot2;
  return true;
}

inline bool eval_contact_position_elements(const float3 &local_position1,
                                           const float3 &local_position2,
                                           const float3 &normal,
                                           const float3 &position1,
                                           const float3 &position2,
                                           const math::Quaternion &rotation1,
                                           const math::Quaternion &rotation2,
                                           float &r_residual,
                                           float3 &r_position_gradient1,
                                           float3 &r_position_gradient2,
                                           float4 &r_rotation_gradient1,
                                           float4 &r_rotation_gradient2)
{
  /* Local positions are relative to colliders.
   * Normal is a fixed shared direction for both participants. */

  /* Contact points are computed by applying the transforms to relative local positions. */
  const float3 contact_point1 = math::transform_point(rotation1, local_position1) + position1;
  const float3 contact_point2 = math::transform_point(rotation2, local_position2) + position2;

  /* Positional constraint for penetration depth along the normal. */
  r_residual = math::dot(contact_point1 - contact_point2, normal);
  /* Only act on contact. */
  const bool active = r_residual < 0.0f;
  if (!active) {
    return false;
  }

  r_position_gradient1 = normal;
  r_position_gradient2 = -r_position_gradient1;
  r_rotation_gradient1 = float4(0.0f, math::cross(local_position1, normal));
  r_rotation_gradient2 = -r_rotation_gradient1;
  return true;
}

inline bool apply_position_contact(const float weight_pos1,
                                   const float weight_pos2,
                                   const float weight_rot1,
                                   const float weight_rot2,
                                   const float3 &local_position1,
                                   const float3 &local_position2,
                                   const float3 &normal,
                                   const float alpha,
                                   float &lambda,
                                   float3 &position1,
                                   float3 &position2,
                                   math::Quaternion &rotation1,
                                   math::Quaternion &rotation2)
{
  constexpr bool linearized_quaternion = true;

  float residual_depth;
  float delta_lambda;
  float3 delta_pos1, delta_pos2;
  float4 delta_rot1, delta_rot2;
  const bool active = eval_position_contact(weight_pos1,
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
                                            rotation2,
                                            residual_depth,
                                            delta_lambda,
                                            delta_pos1,
                                            delta_pos2,
                                            delta_rot1,
                                            delta_rot2);
  if (!active) {
    return false;
  }

  lambda += delta_lambda;
  apply_position_impulse(delta_pos1, position1);
  apply_position_impulse(delta_pos2, position2);
  apply_rotation_impulse<linearized_quaternion>(delta_rot1, rotation1);
  apply_rotation_impulse<linearized_quaternion>(delta_rot2, rotation2);
  return true;
}

/**
 * Velocity contact constraint based on
 * "Detailed Rigid Body Simulation with Extended Position Based Dynamics", Mueller et al., 2020
 */
inline void eval_velocity_contact(const float3 &orig_velocity1,
                                  const float3 &orig_velocity2,
                                  const float3 &orig_angular_velocity1,
                                  const float3 &orig_angular_velocity2,
                                  const float3 &local_position1,
                                  const float3 &local_position2,
                                  const float3 &normal,
                                  const float restitution,
                                  const float friction,
                                  const float lambda_restitution,
                                  const float lambda_friction,
                                  const float3 &velocity1,
                                  const float3 &velocity2,
                                  const float3 &angular_velocity1,
                                  const float3 &angular_velocity2,
                                  const float threshold_normal_velocity,
                                  float &r_residual_restitution,
                                  float &r_residual_friction,
                                  float &r_delta_lambda_restitution,
                                  float &r_delta_lambda_friction,
                                  float3 &r_delta_velocity1,
                                  float3 &r_delta_velocity2,
                                  float3 &r_delta_angular_velocity1,
                                  float3 &r_delta_angular_velocity2)
{
  /* XXX do these need to be considered? */
  UNUSED_VARS(lambda_restitution, lambda_friction);

  /* Compute velocity of the collider contact point. */
  const float3 contact_velocity1 = velocity1 + math::cross(angular_velocity1, local_position1);
  const float3 contact_velocity2 = velocity2 + math::cross(angular_velocity2, local_position2);
  const float3 orig_contact_velocity1 = orig_velocity1 +
                                        math::cross(orig_angular_velocity1, local_position1);
  const float3 orig_contact_velocity2 = orig_velocity2 -
                                        math::cross(orig_angular_velocity2, local_position2);

  /* Relative contact velocity before and after position corrections. */
  const float3 relative_velocity = contact_velocity1 - contact_velocity2;
  const float3 orig_relative_velocity = orig_contact_velocity1 - orig_contact_velocity2;

  /* Decompose into normal and tangential velocity. */
  const float normal_velocity = math::dot(relative_velocity, normal);
  const float orig_normal_velocity = math::dot(orig_relative_velocity, normal);
  const float3 surface_velocity = relative_velocity - normal * normal_velocity;
  /* If normal velocity after update is below jitter threshold avoid any restitution. */
  const bool is_jitter_velocity = (math::abs(normal_velocity) < threshold_normal_velocity);

  r_residual_restitution = orig_normal_velocity;
  r_residual_friction = math::length(surface_velocity);

  /* Kill normal velocity, then add restitution. */
  r_delta_lambda_restitution = is_jitter_velocity ?
                                   0.0f :
                                   -std::min(restitution * r_residual_restitution, 0.0f);
  r_delta_lambda_friction = -friction * r_residual_friction;
  const float3 impulse_restitution = (-normal_velocity + r_delta_lambda_restitution) * normal;
  const float3 impulse_friction = -friction * surface_velocity;
  const float3 impulse = impulse_restitution + impulse_friction;

  r_delta_velocity1 = impulse;
  r_delta_velocity2 = -impulse;
  r_delta_angular_velocity1 = math::cross(local_position1, impulse);
  r_delta_angular_velocity2 = -math::cross(local_position2, impulse);
}

inline void eval_contact_velocity_elements(const float3 &local_position1,
                                           const float3 &local_position2,
                                           const float3 &normal,
                                           const float3 &velocity1,
                                           const float3 &velocity2,
                                           const float3 &angular_velocity1,
                                           const float3 &angular_velocity2,
                                           const float3 &orig_velocity1,
                                           const float3 &orig_velocity2,
                                           const float3 &orig_angular_velocity1,
                                           const float3 &orig_angular_velocity2,
                                           float2 &r_residual,
                                           float4x4 &r_velocity_gradient1,
                                           float4x4 &r_velocity_gradient2,
                                           float4x4 &r_angular_velocity_gradient1,
                                           float4x4 &r_angular_velocity_gradient2)
{
  /* Compute velocity of the collider contact point. */
  const float3 contact_velocity1 = velocity1 + math::cross(angular_velocity1, local_position1);
  const float3 contact_velocity2 = velocity2 + math::cross(angular_velocity2, local_position2);
  const float3 orig_contact_velocity1 = orig_velocity1 +
                                        math::cross(orig_angular_velocity1, local_position1);
  const float3 orig_contact_velocity2 = orig_velocity2 -
                                        math::cross(orig_angular_velocity2, local_position2);

  /* Relative contact velocity before and after position corrections. */
  const float3 relative_velocity = contact_velocity1 - contact_velocity2;
  const float3 orig_relative_velocity = orig_contact_velocity1 - orig_contact_velocity2;

  /* Decompose into normal and tangential velocity. */
  const float normal_velocity = math::dot(relative_velocity, normal);
  const float orig_normal_velocity = math::dot(orig_relative_velocity, normal);
  /* If normal velocity after update is below jitter threshold avoid any restitution. */
  // const bool is_jitter_velocity = (math::abs(normal_velocity) < threshold_normal_velocity);
  float surface_velocity;
  const float3 surface_direction = math::normalize_and_get_length(
      relative_velocity - normal * normal_velocity, surface_velocity);

  r_residual = {orig_normal_velocity, surface_velocity};

  r_velocity_gradient1[0] = float4(normal, 0.0f);
  r_velocity_gradient1[1] = float4(surface_direction, 0.0f);
  r_velocity_gradient2 = -r_velocity_gradient1;
  r_angular_velocity_gradient1[0] = float4(0.0f, math::cross(local_position1, normal));
  r_angular_velocity_gradient1[1] = float4(0.0f, math::cross(local_position1, surface_direction));
  r_angular_velocity_gradient2[0] = -float4(0.0f, math::cross(local_position2, normal));
  r_angular_velocity_gradient2[1] = -float4(0.0f, math::cross(local_position2, surface_direction));
}

inline void apply_velocity_contact(const float3 &orig_velocity1,
                                   const float3 &orig_velocity2,
                                   const float3 &orig_angular_velocity1,
                                   const float3 &orig_angular_velocity2,
                                   const float3 &local_position1,
                                   const float3 &local_position2,
                                   const float3 &normal,
                                   const float restitution,
                                   const float friction,
                                   const float threshold_normal_velocity,
                                   float &lambda_restitution,
                                   float &lambda_friction,
                                   float3 &velocity1,
                                   float3 &velocity2,
                                   float3 &angular_velocity1,
                                   float3 &angular_velocity2)
{
  float residual_restitution, residual_friction;
  float delta_lambda_restitution, delta_lambda_friction;
  float3 delta_vel1, delta_vel2;
  float3 delta_angvel1, delta_angvel2;
  eval_velocity_contact(orig_velocity1,
                        orig_velocity2,
                        orig_angular_velocity1,
                        orig_angular_velocity2,
                        local_position1,
                        local_position2,
                        normal,
                        restitution,
                        friction,
                        lambda_restitution,
                        lambda_friction,
                        velocity1,
                        velocity2,
                        angular_velocity1,
                        angular_velocity2,
                        threshold_normal_velocity,
                        residual_restitution,
                        residual_friction,
                        delta_lambda_restitution,
                        delta_lambda_friction,
                        delta_vel1,
                        delta_vel2,
                        delta_angvel1,
                        delta_angvel2);

  lambda_restitution += delta_lambda_restitution;
  lambda_friction += delta_lambda_friction;
  apply_velocity_impulse(delta_vel1, velocity1);
  apply_velocity_impulse(delta_vel2, velocity2);
  apply_angular_velocity_impulse(delta_angvel1, angular_velocity1);
  apply_angular_velocity_impulse(delta_angvel2, angular_velocity2);
}

/** \} */

}  // namespace blender::nodes::xpbd_constraints
