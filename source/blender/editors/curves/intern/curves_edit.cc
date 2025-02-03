/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 */

#include "BLI_array_utils.hh"
#include "BLI_index_mask_expression.hh"

#include "BKE_anonymous_attribute_id.hh"
#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"

#include "GEO_reorder.hh"

#include "ED_curves.hh"

namespace blender::ed::curves {

bool remove_selection(bke::CurvesGeometry &curves, const bke::AttrDomain selection_domain)
{
  const bke::AttributeAccessor attributes = curves.attributes();
  const VArray<bool> selection = *attributes.lookup_or_default<bool>(
      ".selection", selection_domain, true);
  const int domain_size_orig = attributes.domain_size(selection_domain);
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_bools(selection, memory);
  switch (selection_domain) {
    case bke::AttrDomain::Point:
      curves.remove_points(mask, {});
      break;
    case bke::AttrDomain::Curve:
      curves.remove_curves(mask, {});
      break;
    default:
      BLI_assert_unreachable();
  }

  return attributes.domain_size(selection_domain) != domain_size_orig;
}

static void foreach_content_slice_by_offsets(
    const IndexMask &mask,
    const OffsetIndices<int> offset_indices,
    FunctionRef<void(Span<IndexRange> selected_points, IndexRange slice_points, int slice)> fn)
{
  Vector<IndexRange> ranges;
  Span<int> offset_data = offset_indices.data();

  int slice = 0;

  int range_first = mask.first();
  int range_last = mask.first() - 1;

  mask.foreach_index([&](const int64_t index) {
    if (offset_data[slice + 1] <= index) {
      if (range_last - range_first >= 0) {
        ranges.append(IndexRange::from_begin_end_inclusive(range_first, range_last));
        fn(ranges, offset_indices[slice], slice);
        ranges.clear();
      }
      do {
        ++slice;
      } while (offset_data[slice + 1] <= index);
      range_first = index;
    }
    else if (range_last + 1 != index) {
      ranges.append(IndexRange::from_begin_end_inclusive(range_first, range_last));
      range_first = index;
    }
    range_last = index;
  });

  if (range_last - range_first >= 0) {
    ranges.append(IndexRange::from_begin_end_inclusive(range_first, range_last));
    fn(ranges, offset_indices[slice], slice);
  }
}

static void curve_offsets_from_selection(const Span<IndexRange> selected_points,
                                         const IndexRange points,
                                         const int curve,
                                         const bool cyclic,
                                         Vector<int> &r_new_curve_offsets,
                                         Vector<bool> &r_new_cyclic,
                                         Vector<IndexRange> &r_src_ranges,
                                         Vector<int> &r_dst_offsets,
                                         Vector<int> &r_dst_to_src_curve)
{
  const bool merge_loop = cyclic && selected_points.first().size() < points.size() &&
                          selected_points.first().first() == points.first() &&
                          selected_points.last().last() == points.last();

  int last_dst_offset = r_dst_offsets.last();
  int last_curve_offset = r_new_curve_offsets.last();
  for (const IndexRange range : selected_points.drop_front(merge_loop)) {
    r_src_ranges.append(range);
    last_dst_offset += range.size();
    r_dst_offsets.append(last_dst_offset);
    last_curve_offset += range.size();
    r_new_curve_offsets.append(last_curve_offset);
  };
  if (merge_loop) {
    const IndexRange merge_to_end = selected_points.first();
    r_src_ranges.append(merge_to_end);
    r_dst_offsets.append(last_dst_offset + merge_to_end.size());
    r_new_curve_offsets.last() += merge_to_end.size();
  }
  const int curves_added = selected_points.size() - merge_loop;
  r_dst_to_src_curve.append_n_times(curve, curves_added);
  r_new_cyclic.append_n_times(cyclic && selected_points.first().size() == points.size(),
                              curves_added);
}

void duplicate_points(bke::CurvesGeometry &curves, const IndexMask &mask)
{
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const VArray<bool> src_cyclic = curves.cyclic();

  Vector<int> dst_to_src_curve;
  Vector<int> new_curve_offsets({points_by_curve.data().last()});
  Vector<IndexRange> src_ranges;
  Vector<int> dst_offsets({0});
  Vector<bool> dst_cyclic;
  dst_to_src_curve.reserve(curves.curves_num());
  new_curve_offsets.reserve(curves.curves_num() + 1);
  src_ranges.reserve(curves.curves_num());
  dst_offsets.reserve(curves.curves_num() + 1);
  dst_cyclic.reserve(curves.curves_num());

  /* Add the duplicated curves and points. */
  foreach_content_slice_by_offsets(
      mask,
      points_by_curve,
      [&](Span<IndexRange> ranges_to_duplicate, IndexRange points, int curve) {
        curve_offsets_from_selection(ranges_to_duplicate,
                                     points,
                                     curve,
                                     src_cyclic[curve],
                                     new_curve_offsets,
                                     dst_cyclic,
                                     src_ranges,
                                     dst_offsets,
                                     dst_to_src_curve);
      });

  const int old_curves_num = curves.curves_num();
  const int old_points_num = curves.points_num();
  const int num_curves_to_add = dst_to_src_curve.size();
  const int num_points_to_add = mask.size();

  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  /* Delete selection attribute so that it will not have to be resized. */
  remove_selection_attributes(attributes);

  curves.resize(old_points_num + num_points_to_add, old_curves_num + num_curves_to_add);

  array_utils::copy(new_curve_offsets.as_span(),
                    curves.offsets_for_write().drop_front(old_curves_num));

  /* Transfer curve and point attributes. */
  attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    bke::GSpanAttributeWriter attribute = attributes.lookup_for_write_span(iter.name);
    if (!attribute) {
      return;
    }

    switch (iter.domain) {
      case bke::AttrDomain::Curve: {
        if (iter.name == "cyclic") {
          attribute.finish();
          return;
        }
        bke::attribute_math::gather(
            attribute.span,
            dst_to_src_curve,
            attribute.span.slice(IndexRange(old_curves_num, num_curves_to_add)));
        break;
      }
      case bke::AttrDomain::Point: {
        bke::attribute_math::gather_ranges_to_groups(
            src_ranges.as_span(),
            dst_offsets.as_span(),
            attribute.span,
            attribute.span.slice(IndexRange(old_points_num, num_points_to_add)));
        break;
      }
      default: {
        attribute.finish();
        BLI_assert_unreachable();
        return;
      }
    }

    attribute.finish();
  });

  if (!(src_cyclic.is_single() && !src_cyclic.get_internal_single())) {
    array_utils::copy(dst_cyclic.as_span(), curves.cyclic_for_write().drop_front(old_curves_num));
  }

  curves.update_curve_types();
  curves.tag_topology_changed();

  for (const StringRef selection_name : get_curves_selection_attribute_names(curves)) {
    bke::SpanAttributeWriter<bool> selection = attributes.lookup_or_add_for_write_span<bool>(
        selection_name, bke::AttrDomain::Point);
    selection.span.take_back(num_points_to_add).fill(true);
    selection.finish();
  }
}

void duplicate_curves(bke::CurvesGeometry &curves, const IndexMask &mask)
{
  const int orig_points_num = curves.points_num();
  const int orig_curves_num = curves.curves_num();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  /* Delete selection attribute so that it will not have to be resized. */
  remove_selection_attributes(attributes);

  /* Resize the curves and copy the offsets of duplicated curves into the new offsets. */
  curves.resize(curves.points_num(), orig_curves_num + mask.size());
  const IndexRange orig_curves_range = curves.curves_range().take_front(orig_curves_num);
  const IndexRange new_curves_range = curves.curves_range().drop_front(orig_curves_num);

  MutableSpan<int> offset_data = curves.offsets_for_write();
  offset_indices::gather_selected_offsets(
      OffsetIndices<int>(offset_data.take_front(orig_curves_num + 1)),
      mask,
      orig_points_num,
      offset_data.drop_front(orig_curves_num));
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();

  /* Resize the points array to match the new total point count. */
  curves.resize(points_by_curve.total_size(), curves.curves_num());

  attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    bke::GSpanAttributeWriter attribute = attributes.lookup_for_write_span(iter.name);
    switch (iter.domain) {
      case bke::AttrDomain::Point:
        bke::attribute_math::gather_group_to_group(points_by_curve.slice(orig_curves_range),
                                                   points_by_curve.slice(new_curves_range),
                                                   mask,
                                                   attribute.span,
                                                   attribute.span);
        break;
      case bke::AttrDomain::Curve:
        array_utils::gather(attribute.span, mask, attribute.span.take_back(mask.size()));
        break;
      default:
        BLI_assert_unreachable();
        return;
    }
    attribute.finish();
  });

  curves.update_curve_types();
  curves.tag_topology_changed();

  for (const StringRef selection_name : get_curves_selection_attribute_names(curves)) {
    bke::SpanAttributeWriter<bool> selection = attributes.lookup_or_add_for_write_span<bool>(
        selection_name, bke::AttrDomain::Curve);
    selection.span.take_back(mask.size()).fill(true);
    selection.finish();
  }
}

static IndexRange extend_range(const IndexRange range, const IndexRange universe)
{
  return IndexRange::from_begin_end_inclusive(math::max(range.start() - 1, universe.start()),
                                              math::min(range.one_after_last(), universe.last()));
}

/* Extends each range by one point at both ends of it. Merges adjacent ranges if intersections
 * occur. */
static Vector<IndexRange> extend_and_merge(const Span<IndexRange> ranges,
                                           const IndexRange universe,
                                           const bool cyclic)
{
  const bool must_add_first = cyclic && ranges.last().last() == universe.last() &&
                              ranges.first().first() != universe.first();
  const bool must_add_last = cyclic && ranges.first().first() == universe.first() &&
                             ranges.last().last() != universe.last();

  Vector<IndexRange> out;
  out.append(must_add_first ? IndexRange::from_single(universe.first()) :
                              extend_range(ranges.first(), universe));

  for (const IndexRange range : ranges) {
    const IndexRange extended = extend_range(range, universe);
    IndexRange &last = out.last();
    if (!extended.intersect(last).is_empty()) {
      last = IndexRange::from_begin_end_inclusive(last.start(), extended.last());
    }
    else {
      out.append(extended);
    }
  }
  if (must_add_last) {
    out.append(IndexRange::from_single(universe.last()));
  }

  return out;
}

static void curve_offsets_from_selection(const Span<IndexRange> selected_points,
                                         const IndexRange points,
                                         const int curve,
                                         const VArray<bool> cyclic,
                                         Vector<int> &r_new_curve_offsets,
                                         Vector<bool> &r_new_cyclic,
                                         Vector<IndexRange> &r_src_ranges,
                                         Vector<int> &r_dst_offsets,
                                         Vector<int> &r_curve_map)
{
  IndexRange rolled_range;
  int dropped_front_ranges = 0;

  if (cyclic[curve] && selected_points.first().size() < points.size()) {
    const IndexRange first_range = selected_points.first();
    const IndexRange last_range = selected_points.last();

    if (first_range.first() == points.first() && last_range.last() == points.last()) {
      rolled_range = selected_points.first();
      dropped_front_ranges = 1;
    }
  }

  for (const IndexRange range : selected_points.drop_front(dropped_front_ranges)) {
    r_src_ranges.append(range);
    r_dst_offsets.append(r_dst_offsets.last() + range.size());
    r_new_curve_offsets.append(r_new_curve_offsets.last() + range.size());
  };
  if (!rolled_range.is_empty()) {
    r_src_ranges.append(rolled_range);
    r_dst_offsets.append(r_dst_offsets.last() + rolled_range.size());
    r_new_curve_offsets.last() += rolled_range.size();
  }
  const int curves_added = selected_points.size() - dropped_front_ranges;
  r_curve_map.append_n_times(curve, curves_added);
  r_new_cyclic.append_n_times(cyclic[curve] && selected_points.first().size() == points.size() &&
                                  rolled_range.size() == 0,
                              curves_added);
}

static void foreach_mask_content_slice_by_offsets(
    const IndexMask &mask,
    const OffsetIndices<int> offset_indices,
    FunctionRef<void(Span<IndexRange> selected_points, IndexRange range_points, int range_index)>
        fn)
{
  Vector<IndexRange> ranges;

  int current_offset = 0;

  mask.foreach_range([&](const IndexRange range) {
    IndexRange points = offset_indices[current_offset];

    if (range.first() > points.last()) {
      if (!ranges.is_empty()) {
        fn(ranges, points, current_offset++);
        points = offset_indices[current_offset];
        ranges.clear();
      }
      while (range.first() > points.last()) {
        points = offset_indices[++current_offset];
      }
    }

    IndexRange intersect = points.intersect(range);
    ranges.append(intersect);
    IndexRange range_to_handle = range.drop_front(intersect.size());

    while (!range_to_handle.is_empty()) {
      fn(ranges, points, current_offset++);
      points = offset_indices[current_offset];
      ranges.clear();
      intersect = points.intersect(range_to_handle);
      ranges.append(intersect);
      range_to_handle = range_to_handle.drop_front(intersect.size());
    };
  });

  if (!ranges.is_empty()) {
    fn(ranges, offset_indices[current_offset], current_offset);
  }
}

static void gather_group_to_group(const Span<IndexRange> src_ranges,
                                  const OffsetIndices<int> dst_offsets,
                                  const GSpan src,
                                  GMutableSpan dst)
{
  threading::parallel_for(
      src_ranges.index_range(), GrainSize(512).value, [&](const IndexRange range) {
        for (const int i : range) {
          dst.slice(dst_offsets[i]).copy_from(src.slice(src_ranges[i]));
        }
      });
}

Array<IndexRange> invert_ranges(IndexRange universe, Span<IndexRange> ranges)
{
  const bool contains_first = ranges.first().first() == universe.first();
  const bool contains_last = ranges.last().last() == universe.last();
  Array<IndexRange> inverted(ranges.size() - 1 + contains_first + contains_last);

  int64_t start = contains_first ? ranges.first().one_after_last() : universe.first();
  for (const int r : ranges.index_range().drop_front(contains_first).drop_back(contains_last)) {
    const IndexRange range = ranges[r];
    inverted[r] = IndexRange::from_begin_end(start, range.first());
    start = range.one_after_last();
  }
  if (!contains_last) {
    inverted.last() = IndexRange::from_begin_end(start, universe.one_after_last());
  }
  return inverted;
}

bke::CurvesGeometry split_points(const IndexMask &points_to_split,
                                 const bke::CurvesGeometry &curves)
{
  const OffsetIndices points_by_curve = curves.points_by_curve();
  const VArray<bool> cyclic = curves.cyclic();

  Vector<int> curve_map;
  Vector<int> new_offsets({0});

  Vector<IndexRange> src_ranges;
  Vector<int> dst_offsets({0});
  Vector<bool> new_cyclic;

  IndexMaskMemory memory;

  /* Appends offsets of non selected points, to copy them into sliced existing curves. Their ranges
   * have to be extended to left and right to include edge points of selection ranges. */
  foreach_mask_content_slice_by_offsets(
      points_to_split.complement(curves.points_range(), memory),
      points_by_curve,
      [&](const Span<IndexRange> curve_points_to_preserve,
          const IndexRange points,
          const int curve) {
        /* Every range is extended to left and right by one point. Any resulting intersection
         * is merged. */
        Vector<IndexRange> curve_points_to_preserve_expanded = extend_and_merge(
            curve_points_to_preserve, points, cyclic[curve]);
        curve_offsets_from_selection(curve_points_to_preserve_expanded,
                                     points,
                                     curve,
                                     cyclic,
                                     new_offsets,
                                     new_cyclic,
                                     src_ranges,
                                     dst_offsets,
                                     curve_map);
      });

  const int non_selected_curve_num = new_offsets.size() - 1;

  /* Appends offsets of selected points, those will be copied into newly appended curves. */
  foreach_mask_content_slice_by_offsets(
      points_to_split,
      points_by_curve,
      [&](const Span<IndexRange> curve_points_to_split, const IndexRange points, const int curve) {
        curve_offsets_from_selection(curve_points_to_split,
                                     points,
                                     curve,
                                     cyclic,
                                     new_offsets,
                                     new_cyclic,
                                     src_ranges,
                                     dst_offsets,
                                     curve_map);
      });

  bke::CurvesGeometry new_curves = bke::curves::copy_only_curve_domain(curves);
  new_curves.resize(new_offsets.last(), curve_map.size());
  std::copy_n(new_offsets.data(), new_offsets.size(), new_curves.offsets_for_write().data());
  new_curves.cyclic_for_write().copy_from(new_cyclic);

  const bke::AttributeAccessor src_attributes = curves.attributes();
  bke::MutableAttributeAccessor dst_attributes = new_curves.attributes_for_write();

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic"}),
                         curve_map,
                         dst_attributes);

  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_POINT,
           bke::attribute_filter_from_skip_ref(
               ed::curves::get_curves_selection_attribute_names(curves))))
  {
    gather_group_to_group(
        src_ranges.as_span(), dst_offsets.as_span(), attribute.src, attribute.dst.span);
    attribute.dst.finish();
  };

  new_curves.update_curve_types();
  new_curves.tag_topology_changed();

  OffsetIndices<int> oi = new_offsets.as_span();
  remove_selection_attributes(dst_attributes);
  foreach_selection_attribute_writer(
      new_curves, bke::AttrDomain::Point, [&](bke::GSpanAttributeWriter &selection) {
        for (const int curve : IndexRange(non_selected_curve_num)) {
          fill_selection_false(selection.span.slice(oi[curve]));
        }
      });

  return new_curves;
}

void add_curves(bke::CurvesGeometry &curves, const Span<int> new_sizes)
{
  const int orig_points_num = curves.points_num();
  const int orig_curves_num = curves.curves_num();
  curves.resize(orig_points_num, orig_curves_num + new_sizes.size());

  /* Find the final number of points by accumulating the new */
  MutableSpan<int> new_offsets = curves.offsets_for_write().drop_front(orig_curves_num);
  new_offsets.drop_back(1).copy_from(new_sizes);
  offset_indices::accumulate_counts_to_offsets(new_offsets, orig_points_num);
  /* First, resize the curve domain. */
  curves.resize(curves.offsets().last(), curves.curves_num());

  /* Initialize new attribute values, since #CurvesGeometry::resize() doesn't do that. */
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  bke::fill_attribute_range_default(
      attributes, bke::AttrDomain::Point, {}, curves.points_range().drop_front(orig_points_num));
  bke::fill_attribute_range_default(
      attributes, bke::AttrDomain::Curve, {}, curves.curves_range().drop_front(orig_curves_num));

  curves.update_curve_types();
}

void resize_curves(bke::CurvesGeometry &curves,
                   const IndexMask &curves_to_resize,
                   const Span<int> new_sizes)
{
  if (curves_to_resize.is_empty()) {
    return;
  }
  BLI_assert(curves_to_resize.size() == new_sizes.size());
  bke::CurvesGeometry dst_curves = bke::curves::copy_only_curve_domain(curves);

  IndexMaskMemory memory;
  IndexMask curves_to_copy;
  std::optional<IndexRange> range = curves_to_resize.to_range();
  /* Check if we need to copy some curves over. Write the new sizes into the offsets. */
  if (range && curves.curves_range() == *range) {
    curves_to_copy = {};
    dst_curves.offsets_for_write().drop_back(1).copy_from(new_sizes);
  }
  else {
    curves_to_copy = curves_to_resize.complement(curves.curves_range(), memory);
    offset_indices::copy_group_sizes(
        curves.offsets(), curves_to_copy, dst_curves.offsets_for_write());
    array_utils::scatter(new_sizes, curves_to_resize, dst_curves.offsets_for_write());
  }
  /* Accumulate the sizes written from `new_sizes` into offsets. */
  offset_indices::accumulate_counts_to_offsets(dst_curves.offsets_for_write());

  /* Resize the points domain. */
  dst_curves.resize(dst_curves.offsets().last(), dst_curves.curves_num());

  /* Copy point attributes and default initialize newly added point ranges. */
  const bke::AttrDomain domain(bke::AttrDomain::Point);
  const OffsetIndices<int> src_offsets = curves.points_by_curve();
  const OffsetIndices<int> dst_offsets = dst_curves.points_by_curve();
  const bke::AttributeAccessor src_attributes = curves.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();
  src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.domain != domain || bke::attribute_name_is_anonymous(iter.name)) {
      return;
    }
    const GVArraySpan src = *iter.get(domain);
    const CPPType &type = src.type();
    bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
        iter.name, domain, iter.data_type);
    if (!dst) {
      return;
    }

    curves_to_resize.foreach_index(GrainSize(512), [&](const int curve_i) {
      const IndexRange src_points = src_offsets[curve_i];
      const IndexRange dst_points = dst_offsets[curve_i];
      if (dst_points.size() < src_points.size()) {
        const int src_excees = src_points.size() - dst_points.size();
        dst.span.slice(dst_points).copy_from(src.slice(src_points.drop_back(src_excees)));
      }
      else {
        const int dst_excees = dst_points.size() - src_points.size();
        dst.span.slice(dst_points.drop_back(dst_excees)).copy_from(src.slice(src_points));
        GMutableSpan dst_end_slice = dst.span.slice(dst_points.take_back(dst_excees));
        type.value_initialize_n(dst_end_slice.data(), dst_end_slice.size());
      }
    });
    array_utils::copy_group_to_group(src_offsets, dst_offsets, curves_to_copy, src, dst.span);
    dst.finish();
  });

  dst_curves.update_curve_types();

  /* Move the result into `curves`. */
  curves = std::move(dst_curves);
  curves.tag_topology_changed();
}

void reorder_curves(bke::CurvesGeometry &curves, const Span<int> old_by_new_indices_map)
{
  curves = geometry::reorder_curves_geometry(curves, old_by_new_indices_map, {});
}

}  // namespace blender::ed::curves
