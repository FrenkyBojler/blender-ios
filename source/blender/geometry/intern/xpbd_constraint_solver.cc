/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GEO_xpbd_constraint_solver.hh"

namespace blender::geometry::xpbd_constraint_solver {

void solve_gauss_seidel_one_at_a_time(Span<PointSet> point_sets,
                                      Span<ConstraintSet> constraint_sets)
{
  GaussSeidelSolver solver{point_sets};
  for (const ConstraintSet &constraint_set : constraint_sets) {
    for (const int constraint_i : IndexRange(constraint_set.indices->constraints_num)) {
      constraint_set.evaluator->evaluate_gauss_seidel(solver,
                                                      IndexRange::from_single(constraint_i));
    }
  }
}

}  // namespace blender::geometry::xpbd_constraint_solver
