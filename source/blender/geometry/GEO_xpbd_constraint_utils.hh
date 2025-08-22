/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_cache_mutex.hh"
#include "BLI_index_mask.hh"
#include "BLI_multi_value_map.hh"

#include "GEO_xpbd.hh"

namespace blender::xpbd {

/**
 * Utility to implement a constraint evaluator that automatically works with multiple updaters like
 * #GaussSeidelUpdater.
 *
 * Child classes have to implement the templated #evaluate_single method.
 */
template<typename Child> class TemplatedConstraintSet : public ConstraintSet {
 protected:
  const int constraints_num_;
  const int grain_size_ = 256;

 private:
  mutable CacheMutex independent_masks_mutex_;
  mutable IndexMaskMemory independent_masks_memory_;
  mutable Vector<IndexMask> independent_masks_;

 public:
  TemplatedConstraintSet(int constraints_num, Vector<int> affected_points_refs);

  void evaluate_jacobian_non_deterministic_parallel(NonDeterministicJacobianUpdater &updater,
                                                    ConstraintSetParams &params) const override;
  void evaluate_gauss_seidel_parallel(GaussSeidelUpdater &updater,
                                      ConstraintSetParams &params) const override;
  void evaluate_gauss_seidel_one_at_a_time(GaussSeidelUpdater &updater,
                                           ConstraintSetParams &params) const override;

  Span<IndexMask> get_independent_masks() const;

  virtual Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const = 0;

  /**
   * Evaluate a single constraint using the given updater. This has to be implemented on child
   * classes.
   */
  // template<typename UpdaterT>
  // void evaluate_single(
  //   UpdaterT &updater, ConstraintSetParams &params, const int constraint_i) const;
};

Vector<IndexMask> unary_constraints_to_independent_masks(const Span<int> affected_points,
                                                         IndexMaskMemory &memory);
Vector<IndexMask> binary_constraints_to_independent_masks(const Span<int2> affected_points,
                                                          IndexMaskMemory &memory);
Vector<IndexMask> n_ary_constraints_to_independent_masks(const GroupedSpan<int> affected_points,
                                                         IndexMaskMemory &memory);
Vector<IndexMask> n_ary_constraints_to_independent_masks_multi(
    const GroupedSpan<int> affected_points,
    const GroupedSpan<int> effected_points_refs,
    IndexMaskMemory &memory);

/* -------------------------------------------------------------------- */
/** \name Inline Functions
 * \{ */

template<typename PointID, typename GetConstraintPointsFn>
inline int color_constraints(GetConstraintPointsFn &&get_constraint_points_fn,
                             MutableSpan<int> r_colors)
{
  const int constraints_num = r_colors.size();
  MultiValueMap<PointID, int> constraints_by_point;
  for (const int constraint_i : IndexRange(constraints_num)) {
    for (const PointID point_id : get_constraint_points_fn(constraint_i)) {
      constraints_by_point.add(point_id, constraint_i);
    }
  }
  int colors_num = 0;
  for (const int constraint_i : IndexRange(constraints_num)) {
    Vector<int> used_colors;
    for (const PointID point_id : get_constraint_points_fn(constraint_i)) {
      for (const int other_constraint_i : constraints_by_point.lookup(point_id)) {
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

template<typename PointID, typename GetConstraintPointsFn>
inline Vector<IndexMask> detect_independent_constraints(
    GetConstraintPointsFn &&get_constraint_points_fn,
    const int constraints_num,
    IndexMaskMemory &memory)
{
  if (constraints_num == 0) {
    return {};
  }
  Array<int> colors(constraints_num);
  const int colors_num = color_constraints<PointID>(get_constraint_points_fn, colors);
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
inline Span<IndexMask> TemplatedConstraintSet<Child>::get_independent_masks() const
{
  independent_masks_mutex_.ensure(
      [&]() { independent_masks_ = this->generate_independent_masks(independent_masks_memory_); });
  return independent_masks_;
}

template<typename Child>
inline TemplatedConstraintSet<Child>::TemplatedConstraintSet(int constraints_num,
                                                             Vector<int> affected_points_refs)
    : ConstraintSet(std::move(affected_points_refs)), constraints_num_(constraints_num)
{
}

template<typename Child>
inline void TemplatedConstraintSet<Child>::evaluate_jacobian_non_deterministic_parallel(
    NonDeterministicJacobianUpdater &updater, ConstraintSetParams &params) const
{
  const Child &self = static_cast<const Child &>(*this);
  threading::parallel_for(IndexRange(constraints_num_), grain_size_, [&](const IndexRange range) {
    for (const int constraint_i : range) {
      self.evaluate_single(updater, params, constraint_i);
    }
  });
}

template<typename Child>
inline void TemplatedConstraintSet<Child>::evaluate_gauss_seidel_parallel(
    GaussSeidelUpdater &updater, ConstraintSetParams &params) const
{
  const Child &self = static_cast<const Child &>(*this);
  const Span<IndexMask> constraint_masks = this->get_independent_masks();
  for (const int color_i : constraint_masks.index_range()) {
    const IndexMask &constraint_mask = constraint_masks[color_i];
    constraint_mask.foreach_index(GrainSize(grain_size_), [&](const int constraint_i) {
      self.evaluate_single(updater, params, constraint_i);
    });
  }
}

template<typename Child>
inline void TemplatedConstraintSet<Child>::evaluate_gauss_seidel_one_at_a_time(
    GaussSeidelUpdater &updater, ConstraintSetParams &params) const
{
  const Child &self = static_cast<const Child &>(*this);
  for (const int constraint_i : IndexRange(constraints_num_)) {
    self.evaluate_single(updater, params, constraint_i);
  }
}

/** \} */

}  // namespace blender::xpbd
