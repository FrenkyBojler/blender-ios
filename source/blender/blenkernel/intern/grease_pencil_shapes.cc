/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BLI_map.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_grease_pencil_shapes.hh"

namespace blender::bke::greasepencil {

std::optional<ShapeCache> shape_cache_from_shape_ids(const int num_curves,
                                                     const VArray<int> &shape_ids)
{
  if (num_curves == 0 || shape_ids.is_empty()) {
    return std::nullopt;
  }

  /* The size of each shape. This includes zero-id shapes (which always have a size of 1). */
  Vector<int> all_shape_sizes;
  /* Maps the non-zero shape id to the index of the shape. */
  Map<int, int> all_non_zero_shape_indexing;
  /* The shape id of each shape. The shape id zero can appear more than once, others may appear
   * at most once. */
  Vector<int> all_shape_ids;
  /* The index of the curve if the shape is a zero shape. Otherwise -1. */
  Vector<int> all_zero_shape_curve_indices;

  /* Contains the curve indices for each non-zero shape. */
  Vector<Vector<int>> curve_indices_by_non_zero_shape;
  /* Maps the shape id to the index in the #curve_indices_by_non_zero_shape vector. */
  Map<int, int> non_zero_shape_indexing;

  bool has_non_single_curve_shape = false;
  for (const int curve : IndexRange(num_curves)) {
    const int shape_id = shape_ids[curve];
    if (shape_id == 0) {
      all_shape_sizes.append(1);
      all_shape_ids.append(0);
      all_zero_shape_curve_indices.append(curve);
    }
    /* Try adding non zero shape id to the map. */
    else if (all_non_zero_shape_indexing.add(shape_id, all_shape_sizes.size())) {
      all_shape_sizes.append(1);
      all_shape_ids.append(shape_id);
      /* Not a zero shape. */
      all_zero_shape_curve_indices.append(-1);
    }
    else {
      all_shape_sizes[all_non_zero_shape_indexing.lookup(shape_id)]++;
      has_non_single_curve_shape = true;
    }

    /* Keep track of curve indices for non-zero shapes. */
    if (shape_id != 0) {
      if (non_zero_shape_indexing.add(shape_id, curve_indices_by_non_zero_shape.size())) {
        curve_indices_by_non_zero_shape.append(Vector<int>({curve}));
      }
      else {
        curve_indices_by_non_zero_shape[non_zero_shape_indexing.lookup(shape_id)].append(curve);
      }
    }
  }

  if (!has_non_single_curve_shape) {
    /* All shapes are a single curve. Cache is not needed. */
    return std::nullopt;
  }

  all_shape_sizes.append(0);
  OffsetIndices<int> shape_offsets = offset_indices::accumulate_counts_to_offsets(all_shape_sizes);

  Vector<int> shape_map(num_curves);
  MutableSpan<int> shape_map_span = shape_map.as_mutable_span();
  threading::parallel_for(shape_offsets.index_range(), 4096, [&](const IndexRange range) {
    for (const int shape_i : range) {
      const IndexRange shape_range = shape_offsets[shape_i];
      const bool is_zero_shape = all_shape_ids[shape_i] == 0;
      if (is_zero_shape) {
        const int curve_i = all_zero_shape_curve_indices[shape_i];
        BLI_assert(shape_range.size() == 1);
        shape_map_span[shape_range.first()] = curve_i;
      }
      else {
        const int shape_id = all_shape_ids[shape_i];
        const Span<int> curve_indices =
            curve_indices_by_non_zero_shape[non_zero_shape_indexing.lookup(shape_id)].as_span();
        shape_map_span.slice(shape_range).copy_from(curve_indices);
      }
    }
  });

  ShapeCache shape_cache;
  shape_cache.shape_map = std::move(shape_map);
  shape_cache.shape_offsets = std::move(all_shape_sizes);
  return shape_cache;
}

IndexMask selected_mask_to_shapes(const IndexMask selected_mask,
                                  const bke::CurvesGeometry &curves,
                                  const bke::AttrDomain domain,
                                  IndexMaskMemory &memory)
{
  const bke::AttributeAccessor attributes = curves.attributes();
  const VArray<int> shape_ids = *attributes.lookup<int>("shape_id", bke::AttrDomain::Curve);

  /* If the attribute does not exist then each curves is its own shape. */
  if (!shape_ids) {
    if (domain == AttrDomain::Curve) {
      return selected_mask;
    }
    BLI_assert(domain == AttrDomain::Point);

    Array<bool> selected(curves.points_num());
    selected_mask.to_bools(selected);

    const OffsetIndices points_by_curve = curves.points_by_curve();
    const IndexMask selected_curves = IndexMask::from_predicate(
        curves.curves_range(), GrainSize(512), memory, [&](const int curve_i) {
          const IndexRange points = points_by_curve[curve_i];
          for (const int point_i : points) {
            if (selected[point_i]) {
              return true;
            }
          }
          return false;
        });

    return bke::curves::curve_to_point_selection(points_by_curve, selected_curves, memory);
  }

  VectorSet<int> selected_shape_ids;
  selected_mask.foreach_index([&](const int64_t curve_i) {
    const int shape_id = shape_ids[curve_i];
    if (shape_id != 0) {
      selected_shape_ids.add(shape_id);
    }
  });

  Array<bool> src_selected_curves(curves.curves_num());
  selected_mask.to_bools(src_selected_curves);

  const IndexMask selected_curves = IndexMask::from_predicate(
      curves.curves_range(), GrainSize(4096), memory, [&](const int64_t curve_i) {
        const int shape_id = shape_ids[curve_i];
        if (shape_id == 0) {
          return src_selected_curves[curve_i];
        }
        return selected_shape_ids.contains(shape_id);
      });

  if (domain == AttrDomain::Curve) {
    return selected_curves;
  }
  BLI_assert(domain == AttrDomain::Point);

  return bke::curves::curve_to_point_selection(curves.points_by_curve(), selected_curves, memory);
}

}  // namespace blender::bke::greasepencil
