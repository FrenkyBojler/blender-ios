/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_multi_value_map.hh"

#include "GEO_xpbd_common_constraint_set_indices.hh"
#include "GEO_xpbd_constraint_solver.hh"

namespace blender::geometry::xpbd_constraint_solver {

ConstraintSetIndices::ConstraintSetIndices(const int constraints_num,
                                           Vector<int> target_points_refs)
    : constraints_num(constraints_num), target_points_refs(std::move(target_points_refs))
{
}

Span<IndexMask> ConstraintSetIndices::get_independent_masks() const
{
  independent_masks_mutex_.ensure(
      [&]() { independent_masks_ = generate_independent_masks(independent_masks_memory_); });
  return independent_masks_;
}

Vector<IndexMask> ConstraintSetIndices::generate_independent_masks(
    IndexMaskMemory & /*memory*/) const
{
  /* By default, assume all constants depend on each other. So every mask only contains one
   * constraint. */
  Vector<IndexMask> masks;
  for (const int i : IndexRange(this->constraints_num)) {
    const IndexMask mask = IndexRange::from_single(i);
    masks.append(mask);
  }
  return masks;
}

UnaryConstraintSetIndices::UnaryConstraintSetIndices(const int affected_points_ref_i,
                                                     const Span<int> affected_points)
    : ConstraintSetIndices(affected_points.size(), {affected_points_ref_i}),
      affected_points_ref_i_(affected_points_ref_i),
      affected_points_(affected_points)
{
}

Vector<IndexMask> UnaryConstraintSetIndices::generate_independent_masks(
    IndexMaskMemory &memory) const
{
  return detect_independent_constraints<int>(
      [&](const int constraint_i) { return Span<int>(&this->affected_points_[constraint_i], 1); },
      this->constraints_num,
      memory);
}

BinaryConstraintSetIndices::BinaryConstraintSetIndices(const int affected_points_ref_i,
                                                       const Span<int2> affected_points)
    : ConstraintSetIndices(affected_points.size(), {affected_points_ref_i}),
      affected_points_ref_i_(affected_points_ref_i),
      affected_points_(affected_points)
{
}

Vector<IndexMask> BinaryConstraintSetIndices::generate_independent_masks(
    IndexMaskMemory &memory) const
{
  return detect_independent_constraints<int>(
      [&](const int constraint_i) {
        return Span<int>(&this->affected_points_[constraint_i][0], 2);
      },
      this->affected_points_.size(),
      memory);
}

NAryConstraintSetIndices::NAryConstraintSetIndices(const int affected_points_ref_i,
                                                   const GroupedSpan<int> affected_points)
    : ConstraintSetIndices(affected_points.size(), {affected_points_ref_i}),
      affected_points_ref_i_(affected_points_ref_i),
      affected_points_(affected_points)
{
}

Vector<IndexMask> NAryConstraintSetIndices::generate_independent_masks(
    IndexMaskMemory &memory) const
{
  return detect_independent_constraints<int>(
      [&](const int constraint_i) { return affected_points_[constraint_i]; },
      affected_points_.size(),
      memory);
}

MultiNAryConstraintSetIndices::MultiNAryConstraintSetIndices(GroupedSpan<int> affected_points_refs,
                                                             GroupedSpan<int> affected_points)
    : ConstraintSetIndices(affected_points_refs.size(), {}),
      affected_points_refs_(affected_points_refs),
      affected_points_(affected_points)
{
  BLI_assert(affected_points_refs.size() == affected_points.size());
  for (const int points_ref_i : affected_points_refs_.data) {
    this->target_points_refs.append_non_duplicates(points_ref_i);
  }
}
Vector<IndexMask> MultiNAryConstraintSetIndices::generate_independent_masks(
    IndexMaskMemory &memory) const
{
  return detect_independent_constraints<std::pair<int, int>>(
      [&](const int constraint_i) {
        const Span<int> points_refs = affected_points_refs_[constraint_i];
        const Span<int> points_indices = affected_points_[constraint_i];
        Vector<std::pair<int, int>> affected_points;
        affected_points.reserve(points_indices.size());
        for (const int i : points_indices.index_range()) {
          affected_points.append({points_refs[i], points_indices[i]});
        }
        return affected_points;
      },
      affected_points_.size(),
      memory);
}

ConstraintSet::ConstraintSet(ConstraintSetIndices &indices, ConstraintSetEvaluator &evaluator)
    : indices(&indices), evaluator(&evaluator)
{
}

void solve_gauss_seidel_one_at_a_time(const Span<MutablePointsRef> points_refs,
                                      const Span<ConstraintSet> constraint_sets)
{
  if (constraint_sets.is_empty()) {
    return;
  }
  const int max_constraints_num = std::max_element(
                                      constraint_sets.begin(),
                                      constraint_sets.end(),
                                      [](const ConstraintSet &a, const ConstraintSet &b) {
                                        return a.indices->constraints_num <
                                               b.indices->constraints_num;
                                      })
                                      ->indices->constraints_num;

  Vector<int> constraint_mask(max_constraints_num);
  array_utils::fill_index_range<int>(constraint_mask, 0);

  const Vector<PointsRef> readonly_points_refs = points_refs;
  GaussSeidelUpdater updater{points_refs};

  for (const ConstraintSet &constraint_set : constraint_sets) {
    constraint_set.evaluator->evaluate_serial_gauss_seidel(
        updater,
        readonly_points_refs,
        constraint_mask.as_span().take_front(constraint_set.indices->constraints_num));
  }
}

void solve_jacobian_non_deterministic(const Span<MutablePointsRef> points_refs,
                                      const Span<ConstraintSet> constraint_sets)
{
  using Item = NonDeterministicJacobianUpdater::Item;
  const Vector<PointsRef> readonly_points_refs = points_refs;

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
          constraint_set.evaluator->evaluate_parallel_non_deterministic_jacobian(
              updater, readonly_points_refs, mask);
        }
      });

  threading::parallel_for(points_refs.index_range(), 1, [&](const IndexRange point_set_range) {
    for (const int point_set_i : point_set_range) {
      const Span<Item> items = items_arrays[point_set_i];
      const MutablePointsRef &point_set = points_refs[point_set_i];
      threading::parallel_for(IndexRange(point_set.size()), 512, [&](const IndexRange range) {
        for (const int point_i : range) {
          const Item &item = items[point_i];
          if (item.linear_counter == 0) {
            continue;
          }
          const float relaxation_factor = 1.3f;
          const float3 final_offset = item.linear_offset / item.linear_counter * relaxation_factor;
          point_set.positions[point_i] += final_offset;
        }
        if (!point_set.rotations.is_empty()) {
          for (const int point_i : range) {
            const Item &item = items[point_i];
            if (item.rotation_counter == 0) {
              continue;
            }
            const float4 final_offset = item.rotation_offset / item.rotation_counter;
            math::Quaternion &rotation = point_set.rotations[point_i];
            rotation = apply_rotation_offset(rotation, final_offset);
          }
        }
      });
    }
  });
}

void solve_gauss_seidel_parallel(const Span<MutablePointsRef> points_refs,
                                 const Span<ConstraintSet> constraint_sets)
{
  const Vector<PointsRef> readonly_points_refs = points_refs;
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
            const Span<IndexMask> independent_masks =
                constraint_set->indices->get_independent_masks();
            for (const IndexMask &mask : independent_masks) {
              constraint_set->evaluator->evaluate_parallel_gauss_seidel(
                  updater, readonly_points_refs, mask);
            }
          }
        }
      });

  for (const ConstraintSet *constraint_set : multi_target_constraints) {
    const Span<IndexMask> independent_masks = constraint_set->indices->get_independent_masks();
    for (const IndexMask &mask : independent_masks) {
      constraint_set->evaluator->evaluate_parallel_gauss_seidel(
          updater, readonly_points_refs, mask);
    }
  }
}

}  // namespace blender::geometry::xpbd_constraint_solver
