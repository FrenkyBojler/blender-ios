/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_quaternion_types.hh"

#include "BKE_geometry_set.hh"

namespace blender::geometry::hair_constraints {

struct ConstraintVariables;

/* -------------------------------------------------------------------- */
/** \name Constraint Geometry Setup
 * \{ */

bke::GeometrySet create_position_goal_constraints(const IndexMask &selection,
                                                  const VArray<float> &compliance,
                                                  const VArray<float> &damping,
                                                  const VArray<float3> &goal_position);

bke::GeometrySet create_rotation_goal_constraints(const IndexMask &selection,
                                                  const VArray<float> &compliance,
                                                  const VArray<float> &damping,
                                                  const VArray<math::Quaternion> &goal_rotation);

bke::GeometrySet create_stretch_shear_constraints(const IndexMask &selection,
                                                  const VArray<float> &compliance,
                                                  const VArray<float> &damping,
                                                  const VArray<float3> &rest_position);

bke::GeometrySet create_bend_twist_constraints(const IndexMask &selection,
                                               const VArray<float3> &compliance,
                                               const VArray<float> &damping,
                                               const VArray<math::Quaternion> &rest_rotation);

bke::GeometrySet create_contact_constraints(int collider_index,
                                            const IndexMask &selection,
                                            const VArray<float> &friction,
                                            const VArray<float> &restitution,
                                            const VArray<float> &threshold_normal_velocity,
                                            const VArray<float3> &local_position,
                                            const VArray<float3> &collider_position,
                                            const VArray<float3> &normal);
/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug Recorder
 * \{ */

struct DebugRecorder {
 private:
  bke::GeometrySet geometry_set_;
  bke::GeometryComponent::Type component_type_;

  bke::GeometrySet debug_steps_;

 public:
  DebugRecorder(const bke::GeometrySet &debug_steps);

  void set_geometry(const bke::GeometrySet &geometry_set,
                    bke::GeometryComponent::Type component_type);

  void record_step(const StringRef label,
                   bke::GeometrySet *constraints,
                   const int constraint_type_code,
                   const IndexMask &group_mask,
                   const ConstraintVariables &variables);

  const bke::GeometrySet &debug_steps() const;
};

/** \} */

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

enum class ConstraintType {
  StretchShear,
  BendTwist,
  PositionGoal,
  RotationGoal,
  Contact,
};

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

const ConstraintTypeInfo &get_info(ConstraintType type, bool debug_check);

Span<ConstraintTypeInfo> get_constraint_info(bool debug_output);
Span<ConstraintTypeInfo> get_constraint_info_ordered(bool debug_output);

/** \} */

}  // namespace blender::geometry::hair_constraints
