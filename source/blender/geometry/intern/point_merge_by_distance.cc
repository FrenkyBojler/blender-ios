/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_kdtree.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"

#include "DNA_pointcloud_types.h"

#include "BKE_attribute_math.hh"
#include "BKE_pointcloud.hh"

#include "GEO_point_merge_by_distance.hh"
#include "GEO_randomize.hh"

namespace blender::geometry {

static int roots_by_distance(const Span<float3> positions,
                             const IndexMask &selection,
                             const float merge_distance,
                             MutableSpan<int> r_root_indices)
{
  BLI_assert(r_root_indices.size() == positions.size());
  BLI_assert(selection.min_array_size() <= positions.size());

  KDTree_3d *tree = kdtree_3d_new(selection.size());
  selection.foreach_index_optimized<int64_t>(
      [&](const int64_t i) { kdtree_3d_insert(tree, i, positions[i]); });
  kdtree_3d_balance(tree);

  r_root_indices.fill(-1);
  const int total_merge_ops = kdtree_3d_calc_duplicates_fast(
      tree, merge_distance, false, r_root_indices.data());
  kdtree_3d_free(tree);

  threading::parallel_for(r_root_indices.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      if (r_root_indices[i] == -1) {
        r_root_indices[i] = i;
      }
    }
  });

  return total_merge_ops;
}

PointCloud *point_merge_indices(const PointCloud &src_points,
                                const Span<int> root_indices,
                                const int total_roots,
                                const bke::AttributeFilter &attribute_filter)
{
  const bke::AttributeAccessor src_attributes = src_points.attributes();
  const Span<float3> positions = src_points.positions();
  const int src_size = positions.size();

  /* Create the new point cloud and add it to a temporary component for the attribute API. */
  PointCloud *dst_pointcloud = BKE_pointcloud_new_nomain(total_roots);
  bke::MutableAttributeAccessor dst_attributes = dst_pointcloud->attributes_for_write();

  /* For every source index, find the corresponding index in the result by iterating through the
   * source indices and counting how many merges happened before that point. */
  int merged_points = 0;
  Array<int> src_to_dst_indices(src_size);
  for (const int i : IndexRange(src_size)) {
    src_to_dst_indices[i] = i - merged_points;
    if (root_indices[i] != i) {
      merged_points++;
    }
  }

  /* In order to use a contiguous array as the storage for every destination point's source
   * indices, first the number of source points must be counted for every result point. */
  Array<int> point_merge_counts(total_roots, 0);
  for (const int i : IndexRange(src_size)) {
    const int merge_index = root_indices[i];
    const int dst_index = src_to_dst_indices[merge_index];
    point_merge_counts[dst_index]++;
  }

  /* This array stores an offset into `merge_map` for every result point. */
  Array<int> map_offsets_data(total_roots + 1);
  int offset = 0;
  for (const int i : IndexRange(total_roots)) {
    map_offsets_data[i] = offset;
    offset += point_merge_counts[i];
  }
  map_offsets_data.last() = offset;
  OffsetIndices<int> map_offsets(map_offsets_data);

  point_merge_counts.fill(0);

  /* This array stores all of the source indices for every result point. The size is the source
   * size because every input point is either merged with another or copied directly. */
  Array<int> merge_map_indices(src_size);
  for (const int i : IndexRange(src_size)) {
    const int merge_index = root_indices[i];
    const int dst_index = src_to_dst_indices[merge_index];

    merge_map_indices[map_offsets[dst_index].first() + point_merge_counts[dst_index]] = i;
    point_merge_counts[dst_index]++;
  }

  Set<StringRefNull> attribute_names = src_attributes.all_names();

  /* Transfer the ID attribute if it exists, using the ID of the first merged point. */
  bke::GAttributeReader src_id_attribute = src_attributes.lookup("id");
  if (src_id_attribute && src_id_attribute.domain == bke::AttrDomain::Point &&
      src_id_attribute.varray.type().is<int>())
  {
    VArraySpan<int> src = src_id_attribute.varray.typed<int>();
    bke::SpanAttributeWriter<int> dst = dst_attributes.lookup_or_add_for_write_only_span<int>(
        "id", bke::AttrDomain::Point);

    threading::parallel_for(IndexRange(total_roots), 1024, [&](IndexRange range) {
      for (const int i_dst : range) {
        dst.span[i_dst] = src[map_offsets[i_dst].first()];
      }
    });

    dst.finish();
    attribute_names.remove_contained("id");
  }

  /* Transfer all other attributes. */
  for (const StringRef name : attribute_names) {
    if (attribute_filter.allow_skip(name)) {
      continue;
    }

    bke::GAttributeReader src_attribute = src_attributes.lookup(name);
    const bke::AttrType type = bke::cpp_type_to_attribute_type(src_attribute.varray.type());

    const CommonVArrayInfo info = src_attribute.varray.common_info();
    if (info.type == CommonVArrayInfo::Type::Single) {
      const bke::AttributeInitValue init(GPointer(src_attribute.varray.type(), info.data));
      if (dst_attributes.add(name, bke::AttrDomain::Point, type, init)) {
        continue;
      }
    }

    bke::GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_only_span(
        name, bke::AttrDomain::Point, type);
    bke::attribute_math::mix_groups(
        GVArraySpan(src_attribute.varray), map_offsets, merge_map_indices, dst_attribute.span);
    dst_attribute.finish();
  }

  debug_randomize_point_order(dst_pointcloud);

  return dst_pointcloud;
}

PointCloud *point_merge_by_distance(const PointCloud &src_points,
                                    const float merge_distance,
                                    const IndexMask &selection,
                                    const bke::AttributeFilter &attribute_filter)
{
  Array<int> root_indices(src_points.totpoint);
  const int total_merge_ops = roots_by_distance(
      src_points.positions(), selection, merge_distance, root_indices);
  return point_merge_indices(
      src_points, root_indices.as_span(), src_points.totpoint - total_merge_ops, attribute_filter);
}

}  // namespace blender::geometry
