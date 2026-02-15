/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_multi_value_map.hh"

#include "GEO_xpbd_constraint_coloring.hh"
#include "GEO_xpbd_solve.hh"

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

Vector<IndexMask> all_independent_masks(const int constraints_num)
{
  return {IndexMask(constraints_num)};
}

void solve_gauss_seidel_one_at_a_time(ConstraintSetParams &params,
                                      const Span<ConstraintSet *> constraint_sets)
{
  SolveStrategy strategy{SolveStrategyType::GaussSeidelOneAtATime, params.geometry_refs()};
  for (ConstraintSet *constraint_set : constraint_sets) {
    constraint_set->solve_step(strategy, params);
  }
}

NonDeterministicJacobianUpdater::NonDeterministicJacobianUpdater(
    const Span<GeometryRef> geometry_refs)
    : geometry_refs_(geometry_refs), items_(geometry_refs.size())
{
  for (const int geo_i : geometry_refs.index_range()) {
    const GeometryRef &geometry_ref = geometry_refs[geo_i];
    GeometryItem &geo_item = items_[geo_i];
    const IndexRange range = IndexRange(geometry_ref.positions.size());
    geo_item.range = range;
    geo_item.offsets.reinitialize(range.size());
  }
}

NonDeterministicJacobianUpdater::NonDeterministicJacobianUpdater(Span<GeometryRef> geometry_refs,
                                                                 const int geo_i,
                                                                 const IndexRange range)
    : geometry_refs_(geometry_refs), items_(geometry_refs.size())
{
  GeometryItem &geo_item = items_[geo_i];
  geo_item.range = range;
  geo_item.offsets.reinitialize(range.size());
}

void NonDeterministicJacobianUpdater::apply()
{
  for (const int geo_i : geometry_refs_.index_range()) {
    const GeometryRef &geometry_ref = geometry_refs_[geo_i];
    GeometryItem &geo_item = items_[geo_i];
    if (geo_item.range.is_empty()) {
      continue;
    }
    threading::parallel_for(geo_item.range.index_range(), 512, [&](const IndexRange range) {
      for (const int i : range) {
        const int point_i = i + geo_item.range.start();
        const OffsetItem &item = geo_item.offsets[i];
        if (item.linear_counter > 0) {
          const float relaxation_factor = 1.3f;
          const float3 final_offset = item.linear_offset / item.linear_counter * relaxation_factor;
          geometry_ref.positions[point_i] += final_offset;
        }
      }
      if (!geometry_ref.rotations.is_empty()) {
        for (const int i : range) {
          const int point_i = i + geo_item.range.start();
          const OffsetItem &item = geo_item.offsets[i];
          if (item.rotation_counter == 0) {
            continue;
          }
          const float4 final_offset = item.rotation_offset / item.rotation_counter;
          math::Quaternion &rotation = geometry_ref.rotations[point_i];
          rotation = apply_rotation_offset(rotation, final_offset);
        }
      }
    });
  }
}

SolveStrategy::SolveStrategy(const SolveStrategyType type, Span<GeometryRef> geometry_refs)
    : type(type)
{
  switch (type) {
    case SolveStrategyType::GaussSeidelParallel:
    case SolveStrategyType::GaussSeidelOneAtATime: {
      updater_.emplace(GaussSeidelUpdater(geometry_refs));
      break;
    }
    case SolveStrategyType::JacobianNonDeterministic: {
      updater_.emplace(std::in_place_type_t<NonDeterministicJacobianUpdater>(), geometry_refs);
      break;
    }
  }
}

SolveStrategy::SolveStrategy(const SolveStrategyType type,
                             const Span<GeometryRef> geometry_refs,
                             const int geo_i,
                             const IndexRange range)
    : type(type)
{
  switch (type) {
    case SolveStrategyType::GaussSeidelParallel:
    case SolveStrategyType::GaussSeidelOneAtATime: {
      updater_.emplace(GaussSeidelUpdater(geometry_refs));
      break;
    }
    case SolveStrategyType::JacobianNonDeterministic: {
      updater_.emplace(
          std::in_place_type_t<NonDeterministicJacobianUpdater>(), geometry_refs, geo_i, range);
      break;
    }
  }
}

void SolveStrategy::apply()
{
  if (auto *jacobian_updater = std::get_if<NonDeterministicJacobianUpdater>(&*updater_)) {
    jacobian_updater->apply();
  }
}

void solve_jacobian_non_deterministic(ConstraintSetParams &params,
                                      const Span<ConstraintSet *> constraint_sets)
{
  SolveStrategy strategy{SolveStrategyType::JacobianNonDeterministic, params.geometry_refs()};
  threading::parallel_for(
      constraint_sets.index_range(), 1, [&](const IndexRange constraint_sets_range) {
        for (const int constraint_set_i : constraint_sets_range) {
          ConstraintSet *constraint_set = constraint_sets[constraint_set_i];
          constraint_set->solve_step(strategy, params);
        }
      });
  strategy.apply();
}

void solve_gauss_seidel_parallel(ConstraintSetParams &params,
                                 const Span<ConstraintSet *> constraint_sets)
{
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

  SolveStrategy strategy{SolveStrategyType::GaussSeidelParallel, params.geometry_refs()};

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
