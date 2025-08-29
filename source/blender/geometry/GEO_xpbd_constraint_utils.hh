/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_cache_mutex.hh"
#include "BLI_index_mask.hh"
#include "BLI_multi_value_map.hh"

#include "BLI_resource_scope.hh"
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
  TemplatedConstraintSet(int constraints_num, Vector<int> affected_geo_indices);

  void solve_step(SolveStrategy &strategy, ConstraintSetParams &params) override;

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

class CurveLocalConstraintSet {
 protected:
  int geo_i_;
  OffsetIndices<int> points_by_curve_;

  friend class CurveLocalConstraintSets;

 public:
  CurveLocalConstraintSet(const int geo_i, const OffsetIndices<int> points_by_curve)
      : geo_i_(geo_i), points_by_curve_(points_by_curve)
  {
  }
  virtual ~CurveLocalConstraintSet() = default;

  virtual void solve_step(SolveStrategy &strategy,
                          ConstraintSetParams &params,
                          IndexRange curves_range) = 0;
  virtual int accumulated_task_size(IndexRange curves_range) const;

  int affected_geo_i() const;
  OffsetIndices<int> points_by_curve() const;
};

template<typename Child> class TemplatedCurveLocalConstraintSet : public CurveLocalConstraintSet {
 public:
  TemplatedCurveLocalConstraintSet(const int geo_i, const OffsetIndices<int> points_by_curve);

  void solve_step(SolveStrategy &strategy,
                  ConstraintSetParams &params,
                  IndexRange curves_range) override;
};

class CurveLocalConstraintSets : public ConstraintSet {
 private:
  Vector<CurveLocalConstraintSet *> constraint_sets_;
  OffsetIndices<int> points_by_curve_;

 public:
  CurveLocalConstraintSets(int geo_i, Vector<CurveLocalConstraintSet *> constraint_sets);

  void solve_step(SolveStrategy &strategy, ConstraintSetParams &params) override;
};

Vector<IndexMask> unary_constraints_to_independent_masks(const Span<int> affected_points,
                                                         IndexMaskMemory &memory);
Vector<IndexMask> binary_constraints_to_independent_masks(const Span<int2> affected_points,
                                                          IndexMaskMemory &memory);
Vector<IndexMask> n_ary_constraints_to_independent_masks(const GroupedSpan<int> affected_points,
                                                         IndexMaskMemory &memory);
Vector<IndexMask> n_ary_constraints_to_independent_masks_multi(
    const GroupedSpan<int> affected_geometries,
    const GroupedSpan<int> affected_points,
    IndexMaskMemory &memory);

class ConstraintSetCollector {
 public:
  Vector<ConstraintSet *> general;
  Vector<CurveLocalConstraintSet *> curve_local;

  static Vector<ConstraintSet *> combine(ResourceScope &scope,
                                         const Span<const ConstraintSetCollector *> collectors);
};

/* -------------------------------------------------------------------- */
/** \name Inline Functions
 * \{ */

template<typename PointID, typename GetConstraintPointIdsFn>
inline int color_constraints(GetConstraintPointIdsFn &&get_constraint_point_ids_fn,
                             MutableSpan<int> r_colors)
{
  const int constraints_num = r_colors.size();
  MultiValueMap<PointID, int> constraints_by_point;
  for (const int constraint_i : IndexRange(constraints_num)) {
    for (const PointID point_id : get_constraint_point_ids_fn(constraint_i)) {
      constraints_by_point.add(point_id, constraint_i);
    }
  }
  int colors_num = 0;
  for (const int constraint_i : IndexRange(constraints_num)) {
    Vector<int> used_colors;
    for (const PointID point_id : get_constraint_point_ids_fn(constraint_i)) {
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

template<typename PointID, typename GetConstraintPointIdsFn>
inline Vector<IndexMask> detect_independent_constraints(
    GetConstraintPointIdsFn &&get_constraint_points_fn,
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
                                                             Vector<int> affected_geo_indices)
    : ConstraintSet(std::move(affected_geo_indices)), constraints_num_(constraints_num)
{
}

template<typename Child>
inline void TemplatedConstraintSet<Child>::solve_step(SolveStrategy &strategy,
                                                      ConstraintSetParams &params)
{
  const Child &self = static_cast<const Child &>(*this);
  switch (strategy.type) {
    case SolveStrategyType::GaussSeidelOneAtATime: {
      auto &updater = std::get<GaussSeidelUpdater>(strategy.updater());
      for (const int constraint_i : IndexRange(constraints_num_)) {
        self.evaluate_single(updater, params, constraint_i);
      }
      break;
    }
    case SolveStrategyType::GaussSeidelParallel: {
      auto &updater = std::get<GaussSeidelUpdater>(strategy.updater());
      const Span<IndexMask> constraint_masks = this->get_independent_masks();
      for (const int color_i : constraint_masks.index_range()) {
        const IndexMask &constraint_mask = constraint_masks[color_i];
        constraint_mask.foreach_index(GrainSize(grain_size_), [&](const int constraint_i) {
          self.evaluate_single(updater, params, constraint_i);
        });
      }
      break;
    }
    case SolveStrategyType::JacobianNonDeterministic: {
      auto &updater = std::get<NonDeterministicJacobianUpdater>(strategy.updater());
      threading::parallel_for(
          IndexRange(constraints_num_), grain_size_, [&](const IndexRange range) {
            for (const int constraint_i : range) {
              self.evaluate_single(updater, params, constraint_i);
            }
          });
      break;
    }
  }
}

inline int CurveLocalConstraintSet::affected_geo_i() const
{
  return geo_i_;
}

inline OffsetIndices<int> CurveLocalConstraintSet::points_by_curve() const
{
  return points_by_curve_;
}

template<typename Child>
inline TemplatedCurveLocalConstraintSet<Child>::TemplatedCurveLocalConstraintSet(
    const int geo_i, const OffsetIndices<int> points_by_curve)
    : CurveLocalConstraintSet(geo_i, points_by_curve)
{
}

template<typename Child>
void TemplatedCurveLocalConstraintSet<Child>::solve_step(SolveStrategy &strategy,
                                                         ConstraintSetParams &params,
                                                         IndexRange curves_range)
{
  Child &self = static_cast<Child &>(*this);
  switch (strategy.type) {
    case SolveStrategyType::GaussSeidelOneAtATime:
    case SolveStrategyType::GaussSeidelParallel: {
      auto &updater = std::get<GaussSeidelUpdater>(strategy.updater());
      for (const int curve_i : curves_range) {
        self.evaluate_curve(updater, params, curve_i);
      }
      break;
    }
    case SolveStrategyType::JacobianNonDeterministic: {
      auto &updater = std::get<NonDeterministicJacobianUpdater>(strategy.updater());
      for (const int curve_i : curves_range) {
        self.evaluate_curve(updater, params, curve_i);
      }
      break;
    }
  }
}

/** \} */

}  // namespace blender::xpbd
