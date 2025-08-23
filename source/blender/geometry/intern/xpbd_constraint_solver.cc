/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_multi_value_map.hh"

#include "GEO_xpbd.hh"
#include "GEO_xpbd_constraint_utils.hh"

namespace blender::xpbd {

ConstraintSet::ConstraintSet(Vector<int> affected_geo_indices)
    : affected_geo_indices_(std::move(affected_geo_indices))
{
}

Vector<IndexMask> unary_constraints_to_independent_masks(const Span<int> affected_points,
                                                         IndexMaskMemory &memory)
{
  return detect_independent_constraints<int>(
      [&](const int constraint_i) { return Span<int>(&affected_points[constraint_i], 1); },
      affected_points.size(),
      memory);
}

Vector<IndexMask> binary_constraints_to_independent_masks(const Span<int2> affected_points,
                                                          IndexMaskMemory &memory)
{
  return detect_independent_constraints<int>(
      [&](const int constraint_i) { return Span<int>(&affected_points[constraint_i][0], 2); },
      affected_points.size(),
      memory);
}

Vector<IndexMask> n_ary_constraints_to_independent_masks(const GroupedSpan<int> affected_points,
                                                         IndexMaskMemory &memory)
{
  return detect_independent_constraints<int>(
      [&](const int constraint_i) { return affected_points[constraint_i]; },
      affected_points.size(),
      memory);
}

Vector<IndexMask> n_ary_constraints_to_independent_masks_multi(
    const GroupedSpan<int> affected_geometries,
    const GroupedSpan<int> affected_points,
    IndexMaskMemory &memory)
{
  return detect_independent_constraints<std::pair<int, int>>(
      [&](const int constraint_i) {
        const Span<int> geo_refs = affected_geometries[constraint_i];
        const Span<int> points_indices = affected_points[constraint_i];
        Vector<std::pair<int, int>> point_keys;
        point_keys.reserve(points_indices.size());
        for (const int i : points_indices.index_range()) {
          point_keys.append({geo_refs[i], points_indices[i]});
        }
        return point_keys;
      },
      affected_points.size(),
      memory);
}

void solve_gauss_seidel_one_at_a_time(const Span<GeometryRef> geometry_refs,
                                      const Span<ConstraintSet *> constraint_sets)
{
  ConstraintSetParams params{geometry_refs};
  SolveStrategy strategy{SolveStrategyType::GaussSeidelOneAtATime,
                         GaussSeidelUpdater{geometry_refs}};
  for (ConstraintSet *constraint_set : constraint_sets) {
    constraint_set->solve_step(strategy, params);
  }
}

int CurveLocalConstraintSet::accumulated_task_size(const IndexRange curves_range) const
{
  /* By default, assume that the task size is relative to the number of points in the curves. */
  return points_by_curve_[curves_range].size();
}

CurveLocalConstraintSets::CurveLocalConstraintSets(
    const int geo_i, Vector<CurveLocalConstraintSet *> constraint_sets)
    : ConstraintSet({geo_i}), constraint_sets_(std::move(constraint_sets))
{
  if (!constraint_sets_.is_empty()) {
    points_by_curve_ = constraint_sets_[0]->points_by_curve();
  }
#ifndef NDEBUG
  for (CurveLocalConstraintSet *constraint_set : constraint_sets_) {
    BLI_assert(constraint_set->affected_geo_i() == geo_i);
  }
#endif
}

void CurveLocalConstraintSets::solve_step(SolveStrategy &strategy, ConstraintSetParams &params)
{
  const int curves_num = points_by_curve_.size();
  switch (strategy.type) {
    case SolveStrategyType::GaussSeidelOneAtATime: {
      /* Solve constraints serially without any parallelism. */
      for (CurveLocalConstraintSet *constraint_set : constraint_sets_) {
        constraint_set->solve_step(strategy, params, IndexRange(curves_num));
      }
      break;
    }
    case SolveStrategyType::JacobianNonDeterministic:
    case SolveStrategyType::GaussSeidelParallel: {
      /* Evaluate constraints in parallel. */
      threading::parallel_for(
          IndexRange(curves_num),
          256,
          [&](const IndexRange range) {
            for (CurveLocalConstraintSet *constraint_set : constraint_sets_) {
              constraint_set->solve_step(strategy, params, range);
            }
          },
          threading::accumulated_task_sizes([&](const IndexRange range) {
            int cost = 0;
            for (const CurveLocalConstraintSet *constraint_set : constraint_sets_) {
              cost += constraint_set->accumulated_task_size(range);
            }
            return cost;
          }));
      break;
    }
  }
}

Vector<ConstraintSet *> ConstraintSetCollector::combine(
    ResourceScope &scope, const Span<ConstraintSetCollector *> collectors)
{
  Vector<ConstraintSet *> result;
  MultiValueMap<int, CurveLocalConstraintSet *> curve_local_constraint_sets_by_geometry;
  for (const ConstraintSetCollector *collector : collectors) {
    result.extend(collector->general);
    for (CurveLocalConstraintSet *curve_local_constraint_set : collector->curve_local) {
      const int geo_i = curve_local_constraint_set->affected_geo_i();
      curve_local_constraint_sets_by_geometry.add(geo_i, curve_local_constraint_set);
    }
  }
  for (const auto item : curve_local_constraint_sets_by_geometry.items()) {
    const int geo_i = item.key;
    const Span<CurveLocalConstraintSet *> local_constraint_sets = item.value;
    auto &combined_set = scope.construct<CurveLocalConstraintSets>(geo_i, local_constraint_sets);
    result.append(&combined_set);
  }
  return result;
}

void solve_jacobian_non_deterministic(const Span<GeometryRef> geometry_refs,
                                      const Span<ConstraintSet *> constraint_sets)
{
  using Item = NonDeterministicJacobianUpdater::Item;

  Array<Array<Item>> items_arrays(geometry_refs.size());
  Array<MutableSpan<Item>> items_spans(geometry_refs.size());
  for (const int point_set_i : geometry_refs.index_range()) {
    items_arrays[point_set_i].reinitialize(geometry_refs[point_set_i].size());
    items_spans[point_set_i] = items_arrays[point_set_i];
  }

  ConstraintSetParams params{geometry_refs};
  SolveStrategy strategy{SolveStrategyType::JacobianNonDeterministic,
                         NonDeterministicJacobianUpdater{items_spans}};
  threading::parallel_for(
      constraint_sets.index_range(), 1, [&](const IndexRange constraint_sets_range) {
        for (const int constraint_set_i : constraint_sets_range) {
          ConstraintSet *constraint_set = constraint_sets[constraint_set_i];
          constraint_set->solve_step(strategy, params);
        }
      });

  threading::parallel_for(geometry_refs.index_range(), 1, [&](const IndexRange point_set_range) {
    for (const int point_set_i : point_set_range) {
      const Span<Item> items = items_arrays[point_set_i];
      const GeometryRef &point_set = geometry_refs[point_set_i];
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

void solve_gauss_seidel_parallel(const Span<GeometryRef> geometry_refs,
                                 const Span<ConstraintSet *> constraint_sets)
{
  ConstraintSetParams params{geometry_refs};
  MultiValueMap<int, ConstraintSet *> single_target_constraints_by_geo_index;
  Vector<ConstraintSet *> multi_target_constraints;

  for (ConstraintSet *constraint_set : constraint_sets) {
    const Span<int> affected_geo_refs = constraint_set->get_affected_geo_indices();
    if (affected_geo_refs.is_empty()) {
      continue;
    }
    if (affected_geo_refs.size() == 1) {
      const int geo_set_i = affected_geo_refs[0];
      single_target_constraints_by_geo_index.add(geo_set_i, constraint_set);
    }
    else {
      multi_target_constraints.append(constraint_set);
    }
  }

  Vector<Span<ConstraintSet *>> single_target_constraint_sets;
  for (const Span<ConstraintSet *> constraint_sets :
       single_target_constraints_by_geo_index.values())
  {
    single_target_constraint_sets.append(constraint_sets);
  }

  SolveStrategy strategy{SolveStrategyType::GaussSeidelParallel,
                         GaussSeidelUpdater{geometry_refs}};

  threading::parallel_for(
      single_target_constraint_sets.index_range(), 1, [&](const IndexRange range) {
        for (const int i : range) {
          /* These constraint sets have to be evaluated serially because they effect the same
           * points.*/
          for (ConstraintSet *constraint_set : single_target_constraint_sets[i]) {
            constraint_set->solve_step(strategy, params);
          }
        }
      });

  for (ConstraintSet *constraint_set : multi_target_constraints) {
    constraint_set->solve_step(strategy, params);
  }
}

}  // namespace blender::xpbd
