/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GEO_xpbd_constraint_solver.hh"

namespace blender::geometry::xpbd_constraint_solver {

/**
 * Constraint set indices for unary constraints, i.e. each constraint affects exactly one point.
 * This class is meant for the case when all the points are within the same #PointsRef.
 */
class UnaryConstraintSetIndices : public ConstraintSetIndices {
 private:
  int affected_points_ref_i_;
  Span<int> affected_points_;

 public:
  UnaryConstraintSetIndices(const int affected_points_ref_i, const Span<int> affected_points);
  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override;
};

/**
 * Constraint set indices for binary constraints, i.e. each constraint affects exactly two points.
 * This class is meant for the case when all the points are within the same #PointsRef.
 */
class BinaryConstraintSetIndices : public ConstraintSetIndices {
 private:
  int affected_points_ref_i_;
  Span<int2> affected_points_;

 public:
  BinaryConstraintSetIndices(const int affected_points_ref_i, const Span<int2> affected_points);
  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override;
};

/**
 * For constraints with an arbitrary number of affected points. This class is meant for the case
 * when all points are within the same #PointsRef.
 */
class NAryConstraintSetIndices : public ConstraintSetIndices {
 private:
  int affected_points_ref_i_;
  GroupedSpan<int> affected_points_;

 public:
  NAryConstraintSetIndices(int affected_points_ref_i, GroupedSpan<int> affected_points);
  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override;
};

class MultiNAryConstraintSetIndices : public ConstraintSetIndices {
 private:
  GroupedSpan<int> affected_points_refs_;
  GroupedSpan<int> affected_points_;

 public:
  MultiNAryConstraintSetIndices(GroupedSpan<int> affected_points_refs,
                                GroupedSpan<int> affected_points);
  Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const override;
};

}  // namespace blender::geometry::xpbd_constraint_solver
