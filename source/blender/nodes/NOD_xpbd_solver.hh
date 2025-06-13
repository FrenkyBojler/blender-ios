/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "NOD_geo_hair_constraints.hh"

#include <Eigen/Sparse>

namespace blender::nodes::xpbd_constraints {

using geometry::hair_constraints::ConstraintEvalParams;
using geometry::hair_constraints::ConstraintTypeInfo;
using geometry::hair_constraints::ConstraintVariables;

/* -------------------------------------------------------------------- */
/** \name Solver Parameters
 * \{ */

/* A closure and associated solver group masks. */
struct ConstraintEvalData {
  const ConstraintTypeInfo *type;
  std::optional<bke::GeometrySet> geometry;
  IndexMask constraints;
  Vector<IndexMask> group_masks;
};

struct VariableIndexArrays {
  std::array<Array<int>, 4> position_indices;
  std::array<Array<int>, 4> rotation_indices;
};

void read_constraint_topology(const Span<ConstraintEvalData> constraint_data,
                              MutableSpan<VariableIndexArrays> indices_by_type);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Solver Methods
 * \{ */

struct GlobalSolverSystem {
  /* Linear system of equations for constrained motion. */
  Eigen::SparseMatrix<float> matrix;
  /* Vector of constraint residuals in the current configuration. */
  Eigen::VectorXf target;
  /* Index mapping for active constraints. */
  Array<IndexMask> constraint_mapping;
};

enum class SolverResult {
  /** Computation was successful. */
  Success,
  /** The provided data did not satisfy the prerequisites. */
  NumericalIssue,
  /** Iterative procedure did not converge. */
  NoConvergence,
  /** The inputs are invalid, or the algorithm has been improperly called.
   * When assertions are enabled, such errors trigger an assert. */
  InvalidInput,
};

GlobalSolverSystem build_global_solve_system(const ConstraintEvalParams &params,
                                             const Span<ConstraintEvalData> constraint_data,
                                             const ConstraintVariables &variables,
                                             const bool debug_check,
                                             IndexMaskMemory &memory);

SolverResult solve_global_system(GlobalSolverSystem &&system,
                                 ConstraintVariables &variables,
                                 MutableSpan<ConstraintEvalData> constraint_data,
                                 Eigen::VectorXf *r_solution = nullptr);

void apply_gauss_seidel_positions_group(const ConstraintEvalParams &eval_params,
                                        const ConstraintTypeInfo &constraint_info,
                                        bke::GeometrySet &constraints,
                                        const IndexMask &group_mask,
                                        ConstraintVariables &variables,
                                        const VariableIndexArrays &index_arrays,
                                        IndexMaskMemory &memory);

void apply_gauss_seidel_velocities_group(const ConstraintEvalParams &eval_params,
                                         const ConstraintTypeInfo &constraint_info,
                                         bke::GeometrySet &constraints,
                                         const IndexMask &group_mask,
                                         ConstraintVariables &variables,
                                         const VariableIndexArrays &index_arrays,
                                         IndexMaskMemory &memory);

void add_jacobi_position_deltas(const ConstraintEvalParams &eval_params,
                                const ConstraintTypeInfo &constraint_info,
                                bke::GeometrySet &constraints,
                                const IndexMask &constraints_mask,
                                ConstraintVariables &variables,
                                const VariableIndexArrays &index_arrays,
                                MutableSpan<float3> point_delta_positions,
                                MutableSpan<float4> point_delta_rotations,
                                MutableSpan<int> position_weights,
                                MutableSpan<int> rotation_weights,
                                IndexMaskMemory &memory);

void add_jacobi_velocity_deltas(const ConstraintEvalParams &eval_params,
                                const ConstraintTypeInfo &constraint_info,
                                bke::GeometrySet &constraints,
                                const IndexMask &constraints_mask,
                                ConstraintVariables &variables,
                                const VariableIndexArrays &index_arrays,
                                MutableSpan<float3> point_delta_velocities,
                                MutableSpan<float3> point_delta_angular_velocities,
                                MutableSpan<int> velocity_weights,
                                MutableSpan<int> angular_velocity_weights,
                                IndexMaskMemory &memory);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Residuals
 * \{ */

/** Compute constraint residuals separately, mostly for measuring quality. */
void compute_residuals(const ConstraintEvalParams &params,
                       const ConstraintVariables &variables,
                       MutableSpan<ConstraintEvalData> constraint_data,
                       const StringRef residual_attribute_id);

/** \} */

}  // namespace blender::nodes::xpbd_constraints
