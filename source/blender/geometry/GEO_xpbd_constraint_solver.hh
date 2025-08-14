/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_index_mask.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::geometry::xpbd_constraint_solver {

struct PointSet {
  MutableSpan<float3> positions;
};

class JacobianSolver {
  Vector<Vector<float3>> position_offsets_;

 public:
  void offset_position(const int point_set_i, const int point_i, const float3 &offset)
  {
    position_offsets_[point_set_i][point_i] += offset;
  }
};

class GaussSeidelSolver {
  Span<PointSet> point_sets_;

 public:
  GaussSeidelSolver(Span<PointSet> point_sets) : point_sets_(point_sets) {}

  void offset_position(const int point_set_i, const int point_i, const float3 &offset)
  {
    point_sets_[point_set_i].positions[point_i] += offset;
  }
};

class ConstraintSetEvaluator {
 public:
  virtual ~ConstraintSetEvaluator() = default;

  virtual void evaluate_jacobian(JacobianSolver &solver,
                                 const IndexMask &constraint_mask) const = 0;
  virtual void evaluate_gauss_seidel(GaussSeidelSolver &solver,
                                     const IndexMask &constraint_mask) const = 0;
};

class ConstraintSetIndices {
 public:
  virtual ~ConstraintSetIndices() = default;

  int constraints_num;

  ConstraintSetIndices(const int constraints_num) : constraints_num(constraints_num) {}
};

class UnaryConstraintSetIndices : public ConstraintSetIndices {
 public:
  int point_set_i;
  Span<int> points;

  UnaryConstraintSetIndices(const int point_set_i, const Span<int> points)
      : ConstraintSetIndices(points.size()), point_set_i(point_set_i), points(points)
  {
  }
};

class BinaryConstraintSetIndices : public ConstraintSetIndices {
 public:
  int point_set_i;
  Span<int2> point_pairs;

  BinaryConstraintSetIndices(const int point_set_i, const Span<int2> point_pairs)
      : ConstraintSetIndices(point_pairs.size()),
        point_set_i(point_set_i),
        point_pairs(point_pairs)
  {
  }
};

struct ConstraintSet {
  const ConstraintSetIndices *indices;
  const ConstraintSetEvaluator *evaluator;

  ConstraintSet(ConstraintSetIndices &indices, ConstraintSetEvaluator &evaluator)
      : indices(&indices), evaluator(&evaluator)
  {
  }
};

template<typename Child> class TemplatedConstraintSetEvaluator : public ConstraintSetEvaluator {
  TemplatedConstraintSetEvaluator() = default;
  friend Child;

 public:
  void evaluate_jacobian(JacobianSolver &solver, const IndexMask &constraint_mask) const override
  {
    const Child &self = static_cast<const Child &>(*this);
    self.evaluate(solver, constraint_mask);
  }

  void evaluate_gauss_seidel(GaussSeidelSolver &solver,
                             const IndexMask &constraint_mask) const override
  {
    const Child &self = static_cast<const Child &>(*this);
    self.evaluate(solver, constraint_mask);
  }

  template<typename SolverT> void evaluate(SolverT &solver, const IndexMask &constraint_mask) const
  {
    constraint_mask.foreach_index(GrainSize(256), [&](const int i) {
      const Child &self = static_cast<const Child &>(*this);
      self.evaluate_single(solver, i);
    });
  }
};

void solver_gauss_seidel_one_at_a_time(Span<PointSet> point_sets,
                                       Span<ConstraintSet> constraint_sets);

}  // namespace blender::geometry::xpbd_constraint_solver
