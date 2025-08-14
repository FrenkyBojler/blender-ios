/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_cache_mutex.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_mutex.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::geometry::xpbd_constraint_solver {

/**
 * Mutable reference to the data that is actually being simulated.
 */
struct PointsRef {
  MutableSpan<float3> positions;
  MutableSpan<math::Quaternion> rotations;

  uint64_t size() const;
};

/**
 * Updater that writes the changes directly to the simulated points.
 */
class GaussSeidelUpdater {
 private:
  Span<PointsRef> points_refs_;

 public:
  GaussSeidelUpdater(Span<PointsRef> point_sets);
  void update_position(const int points_ref_i, const int point_i, const float3 &offset);
  void update_rotation(const int points_ref_i, const int point_i, const math::Quaternion &offset);
};

/**
 * Updater that writes that accumulates all changes into a separate array. This is
 * non-deterministic because float addition is not commutative. It mainly exists for testing
 * purposes.
 */
class NonDeterministicJacobianUpdater {
 public:
  struct Item {
    Mutex linear_mutex;
    int linear_counter = 0;
    float3 linear_offset = float3(0.0f);
    Mutex rotation_mutex;
    int rotation_counter = 0;
    float4 rotation_offset = float4(0.0f);
  };

 private:
  Span<MutableSpan<Item>> offsets_;

 public:
  NonDeterministicJacobianUpdater(Span<MutableSpan<Item>> offsets);
  void update_position(const int points_ref_i, const int point_i, const float3 &offset);
  void update_rotation(const int points_ref_i, const int point_i, const math::Quaternion &offset);
};

/**
 * Base class for constraint evaluators. It evaluates batches of constraints and writes back the
 * results using a passed in "updater".
 *
 * Use #TemplatedConstraintSetEvaluator to instantiate the constraint evaluation for each updater
 * automatically. This avoids having to implement separate Jacobian and Gauss Seidel code paths for
 * such constraints.
 */
class ConstraintSetEvaluator {
 public:
  virtual ~ConstraintSetEvaluator() = default;

  /** Evaluate the constraints in the mask. The constraints may be evaluated in parallel. */
  virtual void evaluate(NonDeterministicJacobianUpdater &updater,
                        const IndexMask &constraint_mask) const = 0;
  virtual void evaluate(GaussSeidelUpdater &updater, const IndexMask &constraint_mask) const = 0;
};

/**
 * Utility to implement a constraint evaluator that automatically works with multiple updaters like
 * #GaussSeidelUpdater.
 *
 * Child classes have to implement the templated #evaluate_single method.
 */
template<typename Child> class TemplatedConstraintSetEvaluator : public ConstraintSetEvaluator {
  TemplatedConstraintSetEvaluator() = default;
  friend Child;

 public:
  void evaluate(NonDeterministicJacobianUpdater &updater,
                const IndexMask &constraint_mask) const override;
  void evaluate(GaussSeidelUpdater &updater, const IndexMask &constraint_mask) const override;

  template<typename UpdaterT>
  void evaluate_templated(UpdaterT &updater, const IndexMask &constraint_mask) const;

  /**
   * Evaluate a single constraint using the given updater. This has to be implemented on child
   * classes.
   */
  // template<typename UpdaterT>
  // void evaluate_single(UpdaterT &updater, const int constraint_i) const;
};

/**
 * Has information about which points are effected by a #ConstraintSetEvaluator. This is used by
 * the solver to make decisions about which constraints can be evaluated in parallel.
 */
class ConstraintSetIndices {
 private:
  mutable CacheMutex independent_masks_mutex_;
  mutable IndexMaskMemory independent_masks_memory_;
  mutable Vector<IndexMask> independent_masks_;

 public:
  virtual ~ConstraintSetIndices() = default;

  /** The number of constraints in this set. */
  int constraints_num;
  /** The indices of the affected #PointsRef. */
  Vector<int> target_points_refs;

  ConstraintSetIndices(const int constraints_num, Vector<int> target_points_refs);

  /**
   * Returns index masks where the constraints in each mask are independent, i.e. they can be
   * solved in parallel using a Gauss Seidel solver. This method only care about independentness
   * within this constraint set, not globally across all constraint sets.
   */
  Span<IndexMask> get_independent_masks() const;

 protected:
  virtual Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const;
};

/**
 * A constraint set is the combination of a #ConstraintSetEvaluator and the corresponding
 * #ConstraintSetIndices.
 */
struct ConstraintSet {
  const ConstraintSetIndices *indices;
  const ConstraintSetEvaluator *evaluator;

  ConstraintSet(ConstraintSetIndices &indices, ConstraintSetEvaluator &evaluator);
};

/**
 * Slow but simple iterative Gauss Seidel solver. It evaluates each constraints serially without
 * any parallelism.
 */
void solve_gauss_seidel_one_at_a_time(Span<PointsRef> points_refs,
                                      Span<ConstraintSet> constraint_sets);

/**
 * Fully parallel Jacobian solver, but it is not deterministic. This is mainly for testing
 * purposes.
 */
void solve_jacobian_non_deterministic(Span<PointsRef> points_refs,
                                      Span<ConstraintSet> constraint_sets);

/**
 * A Gauss Seidel solver that attempts to parallelize the evaluation of constraints.
 */
void solve_gauss_seidel_parallel(Span<PointsRef> points_refs, Span<ConstraintSet> constraint_sets);

/* -------------------------------------------------------------------- */
/** \name Inline Functions
 * \{ */

inline uint64_t PointsRef::size() const
{
  return this->positions.size();
}

inline NonDeterministicJacobianUpdater::NonDeterministicJacobianUpdater(
    Span<MutableSpan<Item>> offsets)
    : offsets_(offsets)
{
}

inline void NonDeterministicJacobianUpdater::update_position(const int points_ref_i,
                                                             const int point_i,
                                                             const float3 &offset)
{
  Item &item = offsets_[points_ref_i][point_i];
  std::lock_guard lock(item.linear_mutex);
  item.linear_counter++;
  item.linear_offset += offset;
}

inline void NonDeterministicJacobianUpdater::update_rotation(const int points_ref_i,
                                                             const int point_i,
                                                             const math::Quaternion &offset)
{
  Item &item = offsets_[points_ref_i][point_i];
  std::lock_guard lock(item.rotation_mutex);
  item.rotation_counter++;
  item.rotation_offset += float4(offset);
}

inline GaussSeidelUpdater::GaussSeidelUpdater(Span<PointsRef> point_sets)
    : points_refs_(point_sets)
{
}

inline void GaussSeidelUpdater::update_position(const int points_ref_i,
                                                const int point_i,
                                                const float3 &offset)
{
  points_refs_[points_ref_i].positions[point_i] += offset;
}

inline void GaussSeidelUpdater::update_rotation(const int points_ref_i,
                                                const int point_i,
                                                const math::Quaternion &offset)
{
  math::Quaternion &rotation = points_refs_[points_ref_i].rotations[point_i];
  rotation = math::normalize(math::Quaternion(float4(rotation) + float4(offset)));
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
inline void TemplatedConstraintSetEvaluator<Child>::evaluate(
    NonDeterministicJacobianUpdater &updater, const IndexMask &constraint_mask) const
{
  const Child &self = static_cast<const Child &>(*this);
  self.evaluate_templated(updater, constraint_mask);
}

template<typename Child>
inline void TemplatedConstraintSetEvaluator<Child>::evaluate(
    GaussSeidelUpdater &updater, const IndexMask &constraint_mask) const
{
  const Child &self = static_cast<const Child &>(*this);
  self.evaluate_templated(updater, constraint_mask);
}

template<typename Child>
template<typename UpdaterT>
inline void TemplatedConstraintSetEvaluator<Child>::evaluate_templated(
    UpdaterT &updater, const IndexMask &constraint_mask) const
{
  constraint_mask.foreach_index(GrainSize(256), [&](const int constraint_i) {
    const Child &self = static_cast<const Child &>(*this);
    self.evaluate_single(updater, constraint_i);
  });
}

/** \} */

}  // namespace blender::geometry::xpbd_constraint_solver
