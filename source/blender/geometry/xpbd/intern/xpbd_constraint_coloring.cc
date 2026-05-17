/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_multi_value_map.hh"

#include "BKE_mesh_mapping.hh"

#include "GEO_xpbd_constraint_coloring_utils.hh"

namespace blender::xpbd {

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
inline ConstraintColoring generic_constraint_coloring(
    GetConstraintPointIdsFn &&get_constraint_points_fn,
    const int constraints_num,
    IndexMaskMemory &memory)
{
  if (constraints_num == 0) {
    return {};
  }
  Array<int> colors(constraints_num);
  const int colors_num = color_constraints<PointID>(get_constraint_points_fn, colors);
  Array<Vector<int>> color_indices(colors_num);
  for (const int constraint_i : IndexRange(constraints_num)) {
    color_indices[colors[constraint_i]].append(constraint_i);
  }
  ConstraintColoring coloring;
  for (const int color_i : IndexRange(colors_num)) {
    const IndexMask mask = IndexMask::from_indices<int>(color_indices[color_i], memory);
    coloring.colors.append(mask);
  }
  return coloring;
}

ConstraintColoring color_constraints__unary(const Span<int> affected_points,
                                            IndexMaskMemory &memory)
{
  return generic_constraint_coloring<int>(
      [&](const int constraint_i) { return Span<int>(&affected_points[constraint_i], 1); },
      affected_points.size(),
      memory);
}

template<typename T, typename Values, typename TConvert>
static int64_t max_element_of(const Values elements,
                              const int64_t grain_size,
                              const TConvert &convert)
{
  return threading::parallel_reduce(
             elements.index_range().drop_front(1),
             grain_size,
             std::make_pair(convert(elements.first()), 0),
             [&](const IndexRange range, auto value) {
               for (const int index : range) {
                 auto item_value = convert(elements[index]);
                 if (item_value > value.first) {
                   value.first = std::move(item_value);
                   value.second = index;
                 }
                 else if (item_value == value.first) {
                   value.second = std::min(value.second, index);
                 }
               }
               return value;
             },
             [&](const auto &a, const auto &b) {
               if (a.first < b.first) {
                 return b;
               }
               else if (a.first == b.first) {
                 return std::make_pair(a.first, std::min(a.second, b.second));
               }
               return a;
             })
      .second;
}

ConstraintColoring color_constraints__binary(const Span<int2> affected_points,
                                             IndexMaskMemory &memory)
{
  const Span<int> verts = affected_points.cast<int>();
  const int max_vert_index = max_element_of<int>(verts, 2048, [&](const int i) { return i; });
  const int total_verts = verts[max_vert_index] + 1;

  Array<int> offsets;
  Array<int> indices;
  const GroupedSpan<int> vert_to_edges = bke::mesh::build_vert_to_edge_map(
      affected_points, total_verts, offsets, indices);

  const int max_degree = max_element_of<int>(
      vert_to_edges.index_range(), 2048, [&](const int vert_i) {
        return vert_to_edges[vert_i].size();
      });
  const int max_colors_num = max_degree;

  Vector<bool, 16> color_is_used(max_colors_num, false);
  int max_colors = 0;

  const int constraints_num = affected_points.size();
  Array<int> colors(constraints_num);
  for (const int constraint_i : affected_points.index_range()) {
    for (const int point_id : {affected_points[constraint_i][0], affected_points[constraint_i][1]})
    {
      for (const int other_constraint_i : vert_to_edges[point_id]) {
        if (other_constraint_i >= constraint_i) {
          continue;
        }
        color_is_used.resize(colors[other_constraint_i] + 1, false);
        color_is_used[colors[other_constraint_i]] = true;
      }
    }
    const int best_color = color_is_used.as_span().first_index_try(false);
    color_is_used.as_mutable_span().fill(false);

    if (best_color == -1) {
      max_colors = std::max<int>(max_colors, color_is_used.size() + 1);
      colors[constraint_i] = color_is_used.size();
    }
    else {
      max_colors = std::max<int>(max_colors, color_is_used.size());
      colors[constraint_i] = best_color;
    }
  }

  ConstraintColoring coloring;
  coloring.colors.reinitialize(max_colors);
  IndexMask::from_groups<int>(
      IndexRange(constraints_num),
      memory,
      [&](const int i) { return colors[i]; },
      coloring.colors);
  return coloring;
}

ConstraintColoring color_constraints__n_ary(const GroupedSpan<int> affected_points,
                                            IndexMaskMemory &memory)
{
  return generic_constraint_coloring<int>(
      [&](const int constraint_i) { return affected_points[constraint_i]; },
      affected_points.size(),
      memory);
}

ConstraintColoring color_constraints__all_independent(const int constraints_num)
{
  return ConstraintColoring{{IndexMask(constraints_num)}};
}

}  // namespace blender::xpbd
