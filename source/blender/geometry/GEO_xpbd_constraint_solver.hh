/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_cache_mutex.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_mutex.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::geometry::xpbd_constraint_solver {

struct PointsRef {
  MutableSpan<float3> positions;

  uint64_t size() const;
};

class NonDeterministicParallelJacobianSolver {
 public:
  struct Item {
    Mutex mutex;
    float3 offset = float3(0.0f);
    int counter = 0;
  };

 private:
  Span<MutableSpan<Item>> offsets_;

 public:
  NonDeterministicParallelJacobianSolver(Span<MutableSpan<Item>> offsets);
  void offset_position(const int points_ref_i, const int point_i, const float3 &offset);
};

class GaussSeidelSolver {
 private:
  Span<PointsRef> points_refs_;

 public:
  GaussSeidelSolver(Span<PointsRef> point_sets);
  void offset_position(const int points_ref_i, const int point_i, const float3 &offset);
};

class ConstraintSetEvaluator {
 public:
  virtual ~ConstraintSetEvaluator() = default;

  virtual void evaluate_jacobian_non_deterministic(NonDeterministicParallelJacobianSolver &solver,
                                                   const IndexMask &constraint_mask) const = 0;
  virtual void evaluate_gauss_seidel_parallel(GaussSeidelSolver &solver,
                                              const IndexMask &constraint_mask) const = 0;
};

class ConstraintSetIndices {
 public:
  virtual ~ConstraintSetIndices() = default;

  int constraints_num;
  Vector<int> target_points_refs;

  ConstraintSetIndices(const int constraints_num, Vector<int> target_points_refs);
  virtual void foreach_independent_mask(const FunctionRef<void(const IndexMask &mask)> fn) const;
};

class UnaryConstraintSetIndices : public ConstraintSetIndices {
 private:
  mutable CacheMutex independent_masks_mutex_;
  mutable IndexMaskMemory independent_masks_memory_;
  mutable Vector<IndexMask> independent_masks_;

 public:
  int points_ref_i;
  Span<int> points;

  UnaryConstraintSetIndices(const int points_ref_i, const Span<int> points);
  void foreach_independent_mask(const FunctionRef<void(const IndexMask &mask)> fn) const override;
};

class BinaryConstraintSetIndices : public ConstraintSetIndices {
 private:
  mutable CacheMutex independent_masks_mutex_;
  mutable IndexMaskMemory independent_masks_memory_;
  mutable Vector<IndexMask> independent_masks_;

 public:
  int points_ref_i;
  Span<int2> point_pairs;

  BinaryConstraintSetIndices(const int points_ref_i, const Span<int2> point_pairs);
  void foreach_independent_mask(const FunctionRef<void(const IndexMask &mask)> fn) const override;
};

struct ConstraintSet {
  const ConstraintSetIndices *indices;
  const ConstraintSetEvaluator *evaluator;

  ConstraintSet(ConstraintSetIndices &indices, ConstraintSetEvaluator &evaluator);
};

template<typename Child> class TemplatedConstraintSetEvaluator : public ConstraintSetEvaluator {
  TemplatedConstraintSetEvaluator() = default;
  friend Child;

 public:
  void evaluate_jacobian_non_deterministic(NonDeterministicParallelJacobianSolver &solver,
                                           const IndexMask &constraint_mask) const override;
  void evaluate_gauss_seidel_parallel(GaussSeidelSolver &solver,
                                      const IndexMask &constraint_mask) const override;
  template<typename SolverT>
  void evaluate(SolverT &solver, const IndexMask &constraint_mask) const;
};

void solve_gauss_seidel_one_at_a_time(Span<PointsRef> points_refs,
                                      Span<ConstraintSet> constraint_sets);

void solve_jacobian_non_deterministic(Span<PointsRef> points_refs,
                                      Span<ConstraintSet> constraint_sets);

void solve_gauss_seidel_parallel(Span<PointsRef> points_refs, Span<ConstraintSet> constraint_sets);

/* -------------------------------------------------------------------- */
/** \name Inline Functions
 * \{ */

inline uint64_t PointsRef::size() const
{
  return this->positions.size();
}

inline NonDeterministicParallelJacobianSolver::NonDeterministicParallelJacobianSolver(
    Span<MutableSpan<Item>> offsets)
    : offsets_(offsets)
{
}

inline void NonDeterministicParallelJacobianSolver::offset_position(const int points_ref_i,
                                                                    const int point_i,
                                                                    const float3 &offset)
{
  Item &item = offsets_[points_ref_i][point_i];
  std::lock_guard lock(item.mutex);
  item.counter++;
  item.offset += offset;
}

inline GaussSeidelSolver::GaussSeidelSolver(Span<PointsRef> point_sets) : points_refs_(point_sets)
{
}

inline void GaussSeidelSolver::offset_position(const int points_ref_i,
                                               const int point_i,
                                               const float3 &offset)
{
  points_refs_[points_ref_i].positions[point_i] += offset;
}

template<typename GetConstraintPointsFn>
inline int color_constraints(GetConstraintPointsFn &&get_constraint_points_fn,
                             MutableSpan<int> r_colors)
{
  const int constraints_num = r_colors.size();
  MultiValueMap<int, int> constraints_by_point;
  for (const int constraint_i : IndexRange(constraints_num)) {
    for (const int point_i : get_constraint_points_fn(constraint_i)) {
      constraints_by_point.add(point_i, constraint_i);
    }
  }
  int colors_num = 0;
  for (const int constraint_i : IndexRange(constraints_num)) {
    Vector<int> used_colors;
    for (const int point_i : get_constraint_points_fn(constraint_i)) {
      for (const int other_constraint_i : constraints_by_point.lookup(point_i)) {
        if (other_constraint_i >= constraint_i) {
          continue;
        }
        used_colors.append_non_duplicates(r_colors[other_constraint_i]);
      }
    }
    int best_color = 0;
    while (used_colors.contains(best_color)) {
      best_color++;
    }
    r_colors[constraint_i] = best_color;
    colors_num = std::max(colors_num, best_color + 1);
  }
  return colors_num;
}

template<typename GetConstraintPointsFn>
inline Vector<IndexMask> detect_independent_constraints(
    GetConstraintPointsFn &&get_constraint_points_fn,
    const int constraints_num,
    IndexMaskMemory &memory)
{
  if (constraints_num == 0) {
    return {};
  }
  Array<int> colors(constraints_num);
  const int colors_num = color_constraints(get_constraint_points_fn, colors);
  Array<Vector<int>> masks_indices(colors_num);
  for (const int constraint_i : IndexRange(constraints_num)) {
    masks_indices[colors[constraint_i]].append(constraint_i);
  }
  Vector<IndexMask> masks;
  for (const int color_i : IndexRange(colors_num)) {
    const IndexMask mask = IndexMask::from_indices<int>(masks_indices[color_i], memory);
    masks.append(mask);
  }
  return masks;
}

template<typename Child>
inline void TemplatedConstraintSetEvaluator<Child>::evaluate_jacobian_non_deterministic(
    NonDeterministicParallelJacobianSolver &solver, const IndexMask &constraint_mask) const
{
  const Child &self = static_cast<const Child &>(*this);
  self.evaluate(solver, constraint_mask);
}

template<typename Child>
inline void TemplatedConstraintSetEvaluator<Child>::evaluate_gauss_seidel_parallel(
    GaussSeidelSolver &solver, const IndexMask &constraint_mask) const
{
  const Child &self = static_cast<const Child &>(*this);
  self.evaluate(solver, constraint_mask);
}

template<typename Child>
template<typename SolverT>
inline void TemplatedConstraintSetEvaluator<Child>::evaluate(
    SolverT &solver, const IndexMask &constraint_mask) const
{
  constraint_mask.foreach_index(GrainSize(256), [&](const int constraint_i) {
    const Child &self = static_cast<const Child &>(*this);
    self.evaluate_single(solver, constraint_i);
  });
}

/** \} */

}  // namespace blender::geometry::xpbd_constraint_solver
