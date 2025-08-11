/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_virtual_array.hh"

namespace blender::geometry::xpbd {

class Points {
 private:
  int id_;
  Array<float3> positions_;
  Array<float3> velocities_;
};

using ConstraintsId = int;

class ConstraintsGatherer {
 public:
  ConstraintsId gather_unary(int points_id, Span<int> points);
  ConstraintsId gather_binary(int points_id, Span<int2> point_pairs);
  ConstraintsId gather_binary_segments(int points_id,
                                       OffsetIndices<int> points_groups,
                                       const VArray<bool> &cyclic);
  ConstraintsId gather_binary_multiple_geometries(Span<int> points_id, Span<int> points);
};

class JacobianSolveParams {
 public:
  void solved_unary(int constraint_i, const float3 &delta);
  void solved_binary(int constraint_i, const float3 &delta1, const float3 &delta2);
};

class ConstraintSet {
 private:
 public:
  virtual void solve_jacobian(float delta_time) = 0;
};

class System {
 private:
  Map<int, std::shared_ptr<Points>> points_by_id_;
  Map<int, std::shared_ptr<ConstraintSet>> constraint_sets_by_id_;

 public:
};

struct XPBDSolverParams {
  Map<int, Points *> points_by_id;
  Vector<const ConstraintSet *> constraint_sets;
};

}  // namespace blender::geometry::xpbd
