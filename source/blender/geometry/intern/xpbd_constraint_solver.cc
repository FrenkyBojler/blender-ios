/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GEO_xpbd_constraint_solver.hh"

namespace blender::geometry::xpbd_constraint_solver {

void solve_gauss_seidel_one_at_a_time(const Span<PointSet> point_sets,
                                      const Span<ConstraintSet> constraint_sets)
{
  GaussSeidelSolver solver{point_sets};
  for (const ConstraintSet &constraint_set : constraint_sets) {
    for (const int constraint_i : IndexRange(constraint_set.indices->constraints_num)) {
      constraint_set.evaluator->evaluate_gauss_seidel(solver,
                                                      IndexRange::from_single(constraint_i));
    }
  }
}

void solve_jacobian_non_deterministic(const Span<PointSet> point_sets,
                                      const Span<ConstraintSet> constraint_sets)
{
  using Item = NonDeterministicParallelJacobianSolver::Item;

  Array<Array<Item>> items_arrays(point_sets.size());
  Array<MutableSpan<Item>> items_spans(point_sets.size());
  for (const int point_set_i : point_sets.index_range()) {
    items_arrays[point_set_i].reinitialize(point_sets[point_set_i].size());
    items_spans[point_set_i] = items_arrays[point_set_i];
  }

  NonDeterministicParallelJacobianSolver solver{items_spans};
  threading::parallel_for(
      constraint_sets.index_range(), 1, [&](const IndexRange constraint_sets_range) {
        for (const int constraint_set_i : constraint_sets_range) {
          const ConstraintSet &constraint_set = constraint_sets[constraint_set_i];
          const IndexMask mask = IndexRange(constraint_set.indices->constraints_num);
          constraint_set.evaluator->evaluate_jacobian_non_deterministic(solver, mask);
        }
      });

  threading::parallel_for(point_sets.index_range(), 1, [&](const IndexRange point_set_range) {
    for (const int point_set_i : point_set_range) {
      const Span<Item> items = items_arrays[point_set_i];
      const PointSet &point_set = point_sets[point_set_i];
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

}  // namespace blender::geometry::xpbd_constraint_solver
