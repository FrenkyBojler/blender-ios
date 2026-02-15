/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_index_mask.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_multi_value_map.hh"

namespace blender::xpbd {

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
Vector<IndexMask> all_independent_masks(const int constraints_num);

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
    for (const PointID &point_id : get_constraint_point_ids_fn(constraint_i)) {
      constraints_by_point.add(point_id, constraint_i);
    }
  }
  int colors_num = 0;
  for (const int constraint_i : IndexRange(constraints_num)) {
    Vector<int> used_colors;
    for (const PointID &point_id : get_constraint_point_ids_fn(constraint_i)) {
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

/** \} */

}  // namespace blender::xpbd
