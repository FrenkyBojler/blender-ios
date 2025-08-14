/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_multi_value_map.hh"
#include "GEO_xpbd_constraint_solver.hh"

namespace blender::geometry::xpbd_constraint_solver {

ConstraintSetIndices::ConstraintSetIndices(const int constraints_num,
                                           Vector<int> target_points_refs)
    : constraints_num(constraints_num), target_points_refs(std::move(target_points_refs))
{
}

void ConstraintSetIndices::foreach_independent_mask(
    const FunctionRef<void(const IndexMask &mask)> fn) const
{
  /* By default, assume all constraints depend on each other, so only one element can be
   * processed in parallel. */
  for (const int i : IndexRange(this->constraints_num)) {
    const IndexMask mask = IndexRange::from_single(i);
    fn(mask);
  }
}

UnaryConstraintSetIndices::UnaryConstraintSetIndices(const int points_ref_i,
                                                     const Span<int> points)
    : ConstraintSetIndices(points.size(), {points_ref_i}),
      points_ref_i(points_ref_i),
      points(points)
{
}

void UnaryConstraintSetIndices::foreach_independent_mask(
    const FunctionRef<void(const IndexMask &mask)> fn) const
{
  independent_masks_mutex_.ensure([&]() {
    independent_masks_ = detect_independent_constraints(
        [&](const int constraint_i) { return Span<int>(&this->points[constraint_i], 1); },
        this->constraints_num,
        independent_masks_memory_);
  });
  for (const IndexMask &mask : independent_masks_) {
    fn(mask);
  }
}

BinaryConstraintSetIndices::BinaryConstraintSetIndices(const int points_ref_i,
                                                       const Span<int2> point_pairs)
    : ConstraintSetIndices(point_pairs.size(), {points_ref_i}),
      points_ref_i(points_ref_i),
      point_pairs(point_pairs)
{
}

void BinaryConstraintSetIndices::foreach_independent_mask(
    const FunctionRef<void(const IndexMask &mask)> fn) const
{
  independent_masks_mutex_.ensure([&]() {
    independent_masks_ = detect_independent_constraints(
        [&](const int constraint_i) { return Span<int>(&this->point_pairs[constraint_i][0], 2); },
        this->point_pairs.size(),
        independent_masks_memory_);
  });
  for (const IndexMask &mask : independent_masks_) {
    fn(mask);
  }
}

ConstraintSet::ConstraintSet(ConstraintSetIndices &indices, ConstraintSetEvaluator &evaluator)
    : indices(&indices), evaluator(&evaluator)
{
}

void solve_gauss_seidel_one_at_a_time(const Span<PointsRef> points_refs,
                                      const Span<ConstraintSet> constraint_sets)
{
  GaussSeidelUpdater updater{points_refs};
  for (const ConstraintSet &constraint_set : constraint_sets) {
    for (const int constraint_i : IndexRange(constraint_set.indices->constraints_num)) {
      constraint_set.evaluator->evaluate_gauss_seidel_parallel(
          updater, IndexRange::from_single(constraint_i));
    }
  }
}

void solve_jacobian_non_deterministic(const Span<PointsRef> points_refs,
                                      const Span<ConstraintSet> constraint_sets)
{
  using Item = NonDeterministicJacobianUpdater::Item;

  Array<Array<Item>> items_arrays(points_refs.size());
  Array<MutableSpan<Item>> items_spans(points_refs.size());
  for (const int point_set_i : points_refs.index_range()) {
    items_arrays[point_set_i].reinitialize(points_refs[point_set_i].size());
    items_spans[point_set_i] = items_arrays[point_set_i];
  }

  NonDeterministicJacobianUpdater updater{items_spans};
  threading::parallel_for(
      constraint_sets.index_range(), 1, [&](const IndexRange constraint_sets_range) {
        for (const int constraint_set_i : constraint_sets_range) {
          const ConstraintSet &constraint_set = constraint_sets[constraint_set_i];
          const IndexMask mask = IndexRange(constraint_set.indices->constraints_num);
          constraint_set.evaluator->evaluate_jacobian_non_deterministic(updater, mask);
        }
      });

  threading::parallel_for(points_refs.index_range(), 1, [&](const IndexRange point_set_range) {
    for (const int point_set_i : point_set_range) {
      const Span<Item> items = items_arrays[point_set_i];
      const PointsRef &point_set = points_refs[point_set_i];
      threading::parallel_for(IndexRange(point_set.size()), 512, [&](const IndexRange range) {
        for (const int point_i : range) {
          const Item &item = items[point_i];
          if (item.counter == 0) {
            continue;
          }
          const float relaxation_factor = 1.3f;
          const float3 final_offset = item.offset / item.counter * relaxation_factor;
          point_set.positions[point_i] += final_offset;
        }
      });
    }
  });
}

void solve_gauss_seidel_parallel(const Span<PointsRef> points_refs,
                                 const Span<ConstraintSet> constraint_sets)
{
  MultiValueMap<int, const ConstraintSet *> single_target_constraints_by_point_set;
  Vector<const ConstraintSet *> multi_target_constraints;

  for (const ConstraintSet &constraint_set : constraint_sets) {
    BLI_assert(constraint_set.indices->target_points_refs.size() > 0);
    if (constraint_set.indices->target_points_refs.size() == 1) {
      const int point_set_i = constraint_set.indices->target_points_refs[0];
      single_target_constraints_by_point_set.add(point_set_i, &constraint_set);
    }
    else {
      multi_target_constraints.append(&constraint_set);
    }
  }

  Vector<Span<const ConstraintSet *>> single_target_constraint_sets;
  for (const Span<const ConstraintSet *> constraint_sets :
       single_target_constraints_by_point_set.values())
  {
    single_target_constraint_sets.append(constraint_sets);
  }

  GaussSeidelUpdater updater{points_refs};

  threading::parallel_for(
      single_target_constraint_sets.index_range(), 1, [&](const IndexRange range) {
        for (const int i : range) {
          /* These constraint sets have to be evaluated serially because they effect the same
           * points.*/
          for (const ConstraintSet *constraint_set : single_target_constraint_sets[i]) {
            constraint_set->indices->foreach_independent_mask([&](const IndexMask &mask) {
              constraint_set->evaluator->evaluate_gauss_seidel_parallel(updater, mask);
            });
          }
        }
      });

  for (const ConstraintSet *constraint_set : multi_target_constraints) {
    constraint_set->indices->foreach_independent_mask([&](const IndexMask &mask) {
      constraint_set->evaluator->evaluate_gauss_seidel_parallel(updater, mask);
    });
  }
}

}  // namespace blender::geometry::xpbd_constraint_solver
