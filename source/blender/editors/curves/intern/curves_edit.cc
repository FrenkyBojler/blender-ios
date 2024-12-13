/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 */

#include "BLI_array_utils.hh"
#include "BLI_index_mask_expression.hh"

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

void duplicate_points(bke::CurvesGeometry &curves, const IndexMask &mask)
{
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const VArray<bool> src_cyclic = curves.cyclic();

  Array<bool> points_to_duplicate(curves.points_num());
  mask.to_bools(points_to_duplicate.as_mutable_span());
  const int num_points_to_add = mask.size();

  int curr_dst_point_start = 0;
  Array<int> dst_to_src_point(num_points_to_add);
  Vector<int> dst_curve_counts;
  Vector<int> dst_to_src_curve;
  Vector<bool> dst_cyclic;

  /* Add the duplicated curves and points. */
  for (const int curve_i : curves.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    const Span<bool> curve_points_to_duplicate = points_to_duplicate.as_span().slice(points);
    const bool curve_cyclic = src_cyclic[curve_i];

    /* Note, these ranges start at zero and needed to be shifted by `points.first()` */
    const Vector<IndexRange> ranges_to_duplicate = array_utils::find_all_ranges(
        curve_points_to_duplicate, true);

    if (ranges_to_duplicate.is_empty()) {
      continue;
    }

    const bool is_last_segment_selected = curve_cyclic &&
                                          ranges_to_duplicate.first().first() == 0 &&
                                          ranges_to_duplicate.last().last() == points.size() - 1;
    const bool is_curve_self_joined = is_last_segment_selected && ranges_to_duplicate.size() != 1;
    const bool is_cyclic = ranges_to_duplicate.size() == 1 && is_last_segment_selected;

    const IndexRange range_ids = ranges_to_duplicate.index_range();
    /* Skip the first range because it is joined to the end of the last range. */
    for (const int range_i : ranges_to_duplicate.index_range().drop_front(is_curve_self_joined)) {
      const IndexRange range = ranges_to_duplicate[range_i];

      array_utils::fill_index_range<int>(
          dst_to_src_point.as_mutable_span().slice(curr_dst_point_start, range.size()),
          range.start() + points.first());
      curr_dst_point_start += range.size();

      dst_curve_counts.append(range.size());
      dst_to_src_curve.append(curve_i);
      dst_cyclic.append(is_cyclic);
    }

    /* Join the first range to the end of the last range. */
    if (is_curve_self_joined) {
      const IndexRange first_range = ranges_to_duplicate[range_ids.first()];
      array_utils::fill_index_range<int>(
          dst_to_src_point.as_mutable_span().slice(curr_dst_point_start, first_range.size()),
          first_range.start() + points.first());
      curr_dst_point_start += first_range.size();
      dst_curve_counts[dst_curve_counts.size() - 1] += first_range.size();
    }
  }

  const int old_curves_num = curves.curves_num();
  const int old_points_num = curves.points_num();
  const int num_curves_to_add = dst_to_src_curve.size();

  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  /* Delete selection attribute so that it will not have to be resized. */
  remove_selection_attributes(attributes);

  curves.resize(old_points_num + num_points_to_add, old_curves_num + num_curves_to_add);

  MutableSpan<int> new_curve_offsets = curves.offsets_for_write();
  array_utils::copy(dst_curve_counts.as_span(),
                    new_curve_offsets.drop_front(old_curves_num).drop_back(1));
  offset_indices::accumulate_counts_to_offsets(new_curve_offsets.drop_front(old_curves_num),
                                               old_points_num);

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
        bke::attribute_math::gather(
            attribute.span,
            dst_to_src_point,
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

static IndexMask drop_singles(const IndexMask &mask,
                              const IndexRange universe,
                              const bool cyclic,
                              IndexMaskMemory &memory)
{
  IndexMask to_left = IndexMask::from_difference(
                          mask, IndexMask::from_indices<int>({0}, memory), memory)
                          .shift(-1, memory);
  IndexMask to_right = mask.shift(1, memory);

  index_mask::ExprBuilder builder;
  const index_mask::Expr &shifted_to_sides = builder.merge({&to_left, &to_right});
  IndexMask ensure_cycle_ends;
  if (cyclic && mask.first() == universe.first() && mask.last() == universe.last()) {
    ensure_cycle_ends = IndexMask::from_indices<int64_t>({universe.first(), universe.last()},
                                                         memory);
  }
  return evaluate_expression(
      builder.merge({&builder.intersect({&mask, &shifted_to_sides}), &ensure_cycle_ends}), memory);
}

static IndexRange extend_range(const IndexRange &range, const bool extend)
{
  return extend ? IndexRange::from_begin_end_inclusive(math::max(range.start() - 1, int64_t(0)),
                                                       range.one_after_last()) :
                  range;
}

static void curves_from_selection(const IndexMask &selected_points,
                                  const IndexRange points,
                                  const int curve,
                                  const bool extend_ranges,
                                  const VArray<bool> cyclic,
                                  Vector<int> &new_curve_offsets,
                                  Vector<bool> &new_cyclic,
                                  Vector<int> &src_offsets,
                                  Vector<int> &dst_offsets,
                                  Vector<int> &roll_src_offsets,
                                  Vector<int> &roll_dst_offsets,
                                  Vector<int> &curve_map)
{
  if (selected_points.is_empty()) {
    return;
  }
  int curves_added = 0;
  int roll_by = 0;
  Vector<IndexRange> ranges = selected_points.to_ranges();
  if (cyclic[curve] && selected_points.size() < points.size()) {
    const IndexRange first_range = ranges.first();
    const IndexRange last_range = ranges.last();
    auto setup_roll = [&](const IndexRange rolled_range) {
      roll_by = rolled_range.size();
      roll_src_offsets.append(rolled_range.first());
      roll_src_offsets.append(rolled_range.one_after_last());
      roll_dst_offsets.append(dst_offsets.last() + 0);
      roll_dst_offsets.append(roll_dst_offsets.last() + roll_by);
      dst_offsets.last() += roll_by;
    };
    if (first_range.first() == points.first() && last_range.last() == points.last()) {
      setup_roll(extend_range(ranges.last(), extend_ranges).intersect(points));
      ranges.remove_last();
    }
    else if (extend_ranges && first_range.first() == points.first()) {
      setup_roll(points.take_back(1));
    }
    else if (extend_ranges && last_range.last() == points.last()) {
      setup_roll(extend_range(ranges.last(), extend_ranges).intersect(points));
      ranges.remove_last();
      ranges.prepend(points.take_front(!extend_ranges));
    }
  }

  for (const IndexRange range : ranges) {
    const IndexRange extended_range = extend_range(range, extend_ranges).intersect(points);
    new_curve_offsets.append(new_curve_offsets.last() + extended_range.size());
    src_offsets.append(extended_range.first());
    src_offsets.append(extended_range.one_after_last());
    dst_offsets.append_n_times(dst_offsets.last() + extended_range.size(), 2);
    curves_added++;
  };
  if (roll_by > 0) {
    for (auto &offset : new_curve_offsets.as_mutable_span().take_back(curves_added)) {
      offset += roll_by;
    }
  }
  curve_map.append_n_times(curve, curves_added);
  new_cyclic.append_n_times(
      cyclic[curve] && selected_points.size() == points.size() && roll_by == 0, curves_added);
}

void split_points(const IndexMask &points_to_split,
                  bke::CurvesGeometry &curves,
                  IndexMaskMemory &memory)
{
  const OffsetIndices points_by_curve = curves.points_by_curve();
  const VArray<bool> cyclic = curves.cyclic();

  Vector<int> curve_map;
  Vector<int> new_curve_map;
  Vector<int> new_offsets({0});
  Vector<int> split_curve_offsets({0});

  Vector<int> preserved_src_offsets;
  Vector<int> preserved_dst_offsets({0});
  Vector<int> preserved_roll_src_offsets;
  Vector<int> preserved_roll_dst_offsets;
  Vector<bool> preserved_cyclic;

  Vector<int> split_src_offsets;
  Vector<int> split_dst_offsets({0});
  Vector<int> split_roll_src_offsets;
  Vector<int> split_roll_dst_offsets;
  Vector<bool> split_cyclic;

  for (const int curve : curves.curves_range()) {
    const IndexRange points = points_by_curve[curve];
    const IndexMask preserved_points =
        drop_singles(points_to_split.slice_content(points), points, cyclic[curve], memory)
            .complement(points, memory);
    curves_from_selection(preserved_points,
                          points,
                          curve,
                          true,
                          cyclic,
                          new_offsets,
                          preserved_cyclic,
                          preserved_src_offsets,
                          preserved_dst_offsets,
                          preserved_roll_src_offsets,
                          preserved_roll_dst_offsets,
                          curve_map);

    curves_from_selection(points_to_split.slice_content(points),
                          points,
                          curve,
                          false,
                          cyclic,
                          split_curve_offsets,
                          split_cyclic,
                          split_src_offsets,
                          split_dst_offsets,
                          split_roll_src_offsets,
                          split_roll_dst_offsets,
                          new_curve_map);
  }

  for (int &offset : split_dst_offsets) {
    offset += preserved_dst_offsets.last();
  }

  for (int &offset : split_roll_dst_offsets) {
    offset += preserved_dst_offsets.last();
  }

  const int last_preserved_offset = new_offsets.last();
  for (const int offset : split_curve_offsets.as_span().drop_front(1)) {
    new_offsets.append(last_preserved_offset + offset);
  }

  curve_map.extend(new_curve_map);

  bke::CurvesGeometry new_curves = bke::curves::copy_only_curve_domain(curves);
  new_curves.resize(new_offsets.last(), curve_map.size());
  std::copy_n(new_offsets.data(), new_offsets.size(), new_curves.offsets_for_write().data());
  MutableSpan<bool> new_cyclic = new_curves.cyclic_for_write();
  new_cyclic.take_front(preserved_cyclic.size()).copy_from(preserved_cyclic);
  new_cyclic.take_back(split_cyclic.size()).copy_from(split_cyclic);

  const bke::AttributeAccessor src_attributes = curves.attributes();
  bke::MutableAttributeAccessor dst_attributes = new_curves.attributes_for_write();

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic"}),
                         curve_map,
                         dst_attributes);

  const int max_size = math::max(preserved_src_offsets.size(), split_src_offsets.size());
  const IndexMask groups_to_copy = IndexMask::from_every_nth(2, max_size / 2, 0, memory);
  const IndexMask preserved_to_copy = groups_to_copy.slice(
      IndexRange(preserved_src_offsets.size() / 2));
  const IndexMask preserved_roll_to_copy = groups_to_copy.slice(
      IndexRange(preserved_roll_src_offsets.size() / 2));
  const IndexMask split_to_copy = groups_to_copy.slice(IndexRange(split_src_offsets.size() / 2));
  const IndexMask split_roll_to_copy = groups_to_copy.slice(
      IndexRange(split_roll_src_offsets.size() / 2));

  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_POINT,
           bke::attribute_filter_from_skip_ref(
               ed::curves::get_curves_selection_attribute_names(curves))))
  {
    array_utils::copy_group_to_group(preserved_src_offsets.as_span(),
                                     preserved_dst_offsets.as_span(),
                                     preserved_to_copy,
                                     attribute.src,
                                     attribute.dst.span);
    array_utils::copy_group_to_group(preserved_roll_src_offsets.as_span(),
                                     preserved_roll_dst_offsets.as_span(),
                                     preserved_roll_to_copy,
                                     attribute.src,
                                     attribute.dst.span);
    array_utils::copy_group_to_group(split_src_offsets.as_span(),
                                     split_dst_offsets.as_span(),
                                     split_to_copy,
                                     attribute.src,
                                     attribute.dst.span);
    array_utils::copy_group_to_group(split_roll_src_offsets.as_span(),
                                     split_roll_dst_offsets.as_span(),
                                     split_roll_to_copy,
                                     attribute.src,
                                     attribute.dst.span);
    attribute.dst.finish();
  };

  foreach_selection_attribute_writer(
      new_curves, bke::AttrDomain::Curve, [&](bke::GSpanAttributeWriter &selection) {
        fill_selection_false(selection.span.drop_back(split_curve_offsets.size() - 1));
        fill_selection_true(selection.span.take_back(split_curve_offsets.size() - 1));
      });

  new_curves.update_curve_types();
  new_curves.tag_topology_changed();

  curves = std::move(new_curves);
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
