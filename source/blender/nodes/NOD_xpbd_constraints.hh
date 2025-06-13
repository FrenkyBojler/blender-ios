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

/* -------------------------------------------------------------------- */
/** \name Constraint Bundle Access
 * \{ */

const SocketInterfaceKey &constraint_type_to_socket_key(ConstraintType type);
ConstraintType socket_key_to_constraint_type(const SocketInterfaceKey &key);

struct ConstraintBundleItems {
  bke::GeometrySet stretch_constraints;
  bke::GeometrySet bending_constraints;
  bke::GeometrySet position_constraints;
  bke::GeometrySet rotation_constraints;
  bke::GeometrySet contact_constraints;
};

void set_constraints(BundlePtr &bundle_ptr, ConstraintType type, const bke::GeometrySet &geometry);
bke::GeometrySet lookup_constraints(const Bundle &bundle, ConstraintType type);
BundlePtr combine_constraint_bundle(const ConstraintBundleItems &items);
void separate_constraint_bundle(const Bundle &bundle, ConstraintBundleItems &items);

/** \} */

}  // namespace blender::nodes::xpbd_constraints
