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
                                      const Span<const ConstraintSet *> constraint_sets)
{
  ConstraintSetParams params{geometry_refs};
  GaussSeidelUpdater updater{geometry_refs};
  for (const ConstraintSet *constraint_set : constraint_sets) {
    constraint_set->evaluate_gauss_seidel_one_at_a_time(updater, params);
  }
}

void solve_jacobian_non_deterministic(const Span<GeometryRef> geometry_refs,
                                      const Span<const ConstraintSet *> constraint_sets)
{
  using Item = NonDeterministicJacobianUpdater::Item;
  ConstraintSetParams params{geometry_refs};

  Array<Array<Item>> items_arrays(geometry_refs.size());
  Array<MutableSpan<Item>> items_spans(geometry_refs.size());
  for (const int point_set_i : geometry_refs.index_range()) {
    items_arrays[point_set_i].reinitialize(geometry_refs[point_set_i].size());
    items_spans[point_set_i] = items_arrays[point_set_i];
  }

  NonDeterministicJacobianUpdater updater{items_spans};
  threading::parallel_for(
      constraint_sets.index_range(), 1, [&](const IndexRange constraint_sets_range) {
        for (const int constraint_set_i : constraint_sets_range) {
          const ConstraintSet *constraint_set = constraint_sets[constraint_set_i];
          constraint_set->evaluate_jacobian_non_deterministic_parallel(updater, params);
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
                                 const Span<const ConstraintSet *> constraint_sets)
{
  ConstraintSetParams params{geometry_refs};
  MultiValueMap<int, const ConstraintSet *> single_target_constraints_by_geo_index;
  Vector<const ConstraintSet *> multi_target_constraints;

  for (const ConstraintSet *constraint_set : constraint_sets) {
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

  Vector<Span<const ConstraintSet *>> single_target_constraint_sets;
  for (const Span<const ConstraintSet *> constraint_sets :
       single_target_constraints_by_geo_index.values())
  {
    single_target_constraint_sets.append(constraint_sets);
  }

  GaussSeidelUpdater updater{geometry_refs};

  threading::parallel_for(
      single_target_constraint_sets.index_range(), 1, [&](const IndexRange range) {
        for (const int i : range) {
          /* These constraint sets have to be evaluated serially because they effect the same
           * points.*/
          for (const ConstraintSet *constraint_set : single_target_constraint_sets[i]) {
            constraint_set->evaluate_gauss_seidel_parallel(updater, params);
          }
        }
      });

  for (const ConstraintSet *constraint_set : multi_target_constraints) {
    constraint_set->evaluate_gauss_seidel_parallel(updater, params);
  }
}

}  // namespace blender::xpbd
