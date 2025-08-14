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

  /** Cache for #get_independent_masks. */
  mutable CacheMutex independent_masks_mutex_;
  mutable IndexMaskMemory independent_masks_memory_;
  mutable Vector<IndexMask> independent_masks_;

 public:
  UnaryConstraintSetIndices(const int affected_points_ref_i, const Span<int> affected_points);
  Span<IndexMask> get_independent_masks() const override;
};

/**
 * Constraint set indices for binary constraints, i.e. each constraint affects exactly two points.
 * This class is meant for the case when all the points are within the same #PointsRef.
 */
class BinaryConstraintSetIndices : public ConstraintSetIndices {
 private:
  int affected_points_ref_i_;
  Span<int2> affected_points_;

  /** Cache for #get_independent_masks. */
  mutable CacheMutex independent_masks_mutex_;
  mutable IndexMaskMemory independent_masks_memory_;
  mutable Vector<IndexMask> independent_masks_;

 public:
  BinaryConstraintSetIndices(const int affected_points_ref_i, const Span<int2> affected_points);
  Span<IndexMask> get_independent_masks() const override;
};

}  // namespace blender::geometry::xpbd_constraint_solver
