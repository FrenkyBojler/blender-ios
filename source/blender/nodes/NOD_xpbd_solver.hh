/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "NOD_xpbd_constraints.hh"

#include <Eigen/Sparse>

namespace blender::nodes::xpbd_constraints {

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

/* A closure and associated solver group masks. */
struct ConstraintEvalData {
  const ConstraintTypeInfo *type;
  std::optional<bke::GeometrySet> geometry;
  IndexMask constraints;
  Vector<IndexMask> group_masks;
};

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

GlobalSolverSystem build_global_solve_system(const ConstraintEvalParams &params,
                                             const Span<ConstraintEvalData> constraint_data,
                                             const ConstraintVariables &variables,
                                             const bool debug_check,
                                             IndexMaskMemory &memory);

void solve_global_system(const GlobalSolverSystem &system,
                         ConstraintVariables &variables,
                         MutableSpan<ConstraintEvalData> constraint_data);

void apply_gauss_seidel_positions_group(const ConstraintEvalParams &eval_params,
                                        const ConstraintTypeInfo &constraint_info,
                                        bke::GeometrySet &constraints,
                                        const IndexMask &group_mask,
                                        ConstraintVariables &variables,
                                        IndexMaskMemory &memory);

void apply_gauss_seidel_velocities_group(const ConstraintEvalParams &eval_params,
                                         const ConstraintTypeInfo &constraint_info,
                                         bke::GeometrySet &constraints,
                                         const IndexMask &group_mask,
                                         ConstraintVariables &variables,
                                         IndexMaskMemory &memory);

void add_jacobi_position_deltas(const ConstraintEvalParams &eval_params,
                                const ConstraintTypeInfo &constraint_info,
                                bke::GeometrySet &constraints,
                                const IndexMask &constraints_mask,
                                ConstraintVariables &variables,
                                MutableSpan<float3> point_delta_positions,
                                MutableSpan<float4> point_delta_rotations,
                                MutableSpan<int> point_weights,
                                IndexMaskMemory &memory);

void add_jacobi_velocity_deltas(const ConstraintEvalParams &eval_params,
                                const ConstraintTypeInfo &constraint_info,
                                bke::GeometrySet &constraints,
                                const IndexMask &constraints_mask,
                                ConstraintVariables &variables,
                                MutableSpan<float3> point_delta_velocities,
                                MutableSpan<float3> point_delta_angular_velocities,
                                MutableSpan<int> point_weights,
                                IndexMaskMemory &memory);

/** \} */

}  // namespace blender::nodes::xpbd_constraints
