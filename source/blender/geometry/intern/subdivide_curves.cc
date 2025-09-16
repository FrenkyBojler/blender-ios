/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_deform.hh"

#include "BLI_array_utils.hh"
#include "BLI_task.hh"

#include "GEO_subdivide_curves.hh"

namespace blender::geometry {

static void calculate_result_offsets(const bke::CurvesGeometry &src_curves,
                                     const IndexMask &selection,
                                     const IndexMask &unselected,
                                     const VArray<int> &cuts,
                                     const Span<bool> cyclic,
                                     const bool has_nurbs,
                                     MutableSpan<int> dst_curve_offsets,
                                     MutableSpan<int> dst_point_offsets)
{
  /* Fill the array with each curve's point count, then accumulate them to the offsets. */
  const OffsetIndices src_points_by_curve = src_curves.points_by_curve();
  offset_indices::copy_group_sizes(src_points_by_curve, unselected, dst_curve_offsets);

  /* NURBS */
  const VArray<int8_t> &curve_type = src_curves.curve_types();
  const VArraySpan<int8_t> nurbs_order = src_curves.nurbs_orders();
  const VArraySpan<int8_t> knot_modes = src_curves.nurbs_knots_modes();
  const Span<float> src_custom_knots = src_curves.nurbs_custom_knots();
  const OffsetIndices<int> src_custom_knots_by_curve = src_curves.nurbs_custom_knots_by_curve();

  selection.foreach_index(GrainSize(1024), [&](const int curve_i) {
    const bool is_cyclic = cyclic[curve_i];
    const IndexRange src_points = src_points_by_curve[curve_i];
    const IndexRange src_segments = bke::curves::per_curve_point_offsets_range(src_points,
                                                                               curve_i);

    MutableSpan<int> point_offsets = dst_point_offsets.slice(src_segments);
    MutableSpan<int> point_counts = point_offsets.drop_back(1);

    if (src_points.size() == 1) {
      point_counts.first() = 1;
    }
    else if (has_nurbs && curve_type[curve_i] == CURVE_TYPE_NURBS) {
      /* NURBS curves need to be handled differently as segments are not formed for every
       * control point. Implementation reads only the first N entries from the `cuts`parameter,
       * inserting control points only in the N 'evaluated spans' which forms segments.
       */
      KnotsMode mode = KnotsMode(knot_modes[curve_i]);
      const int8_t order = nurbs_order[curve_i];

      const int knots_num = bke::curves::nurbs::knots_num(src_points.size(), order, is_cyclic);
      Vector<float, 32> knots(knots_num);
      bke::curves::nurbs::load_curve_knots(mode,
                                           src_points.size(),
                                           order,
                                           is_cyclic,
                                           src_custom_knots_by_curve[curve_i],
                                           src_custom_knots,
                                           knots);
      Vector<int> span_offsets;
      bke::curves::nurbs::find_spans(
          src_points.size(), order, cyclic[curve_i], knots, span_offsets);

      point_counts.fill(1);

      const int shift = order / 2;
      int cut_index = 0;
      for (const int i : span_offsets.index_range()) {
        point_counts[span_offsets[i] - shift] = std::max(cuts[cut_index++], 0) + 1;
      }
    }
    else {
      cuts.materialize_compressed(src_points, point_counts);
      for (int &count : point_counts) {
        /* Make sure there at least one cut, and add one for the existing point. */
        count = std::max(count, 0) + 1;
      }
      if (!cyclic[curve_i]) {
        /* The last point only has a segment to be subdivided if the curve isn't cyclic. */
        point_counts.last() = 1;
      }
    }

    offset_indices::accumulate_counts_to_offsets(point_offsets);
    dst_curve_offsets[curve_i] = point_offsets.last();
  });
  offset_indices::accumulate_counts_to_offsets(dst_curve_offsets);
}

template<typename T>
static inline void linear_interpolation(const T &a, const T &b, MutableSpan<T> dst)
{
  dst.first() = a;
  const float step = 1.0f / dst.size();
  for (const int i : dst.index_range().drop_front(1)) {
    dst[i] = bke::attribute_math::mix2(i * step, a, b);
  }
}

template<typename T>
static void interpolate_attribute_linear(const OffsetIndices<int> curve_offsets,
                                         const Span<T> curve_src,
                                         MutableSpan<T> curve_dst)
{
  threading::parallel_for(curve_src.index_range().drop_back(1), 1024, [&](IndexRange range) {
    for (const int i : range) {
      const IndexRange segment_points = curve_offsets[i];
      linear_interpolation(curve_src[i], curve_src[i + 1], curve_dst.slice(segment_points));
    }
  });

  MutableSpan<T> dst_last_segment = curve_dst.take_back(
      curve_offsets[curve_src.index_range().last()].size());
  linear_interpolation(curve_src.last(), curve_src.first(), dst_last_segment);
}

template<typename T>
static void subdivide_attribute_linear(const OffsetIndices<int> src_points_by_curve,
                                       const OffsetIndices<int> dst_points_by_curve,
                                       const IndexMask &selection,
                                       const Span<int> all_point_offsets,
                                       const Span<T> src,
                                       MutableSpan<T> dst)
{
  selection.foreach_index(GrainSize(512), [&](const int curve_i) {
    const IndexRange src_points = src_points_by_curve[curve_i];
    const IndexRange dst_points = dst_points_by_curve[curve_i];

    const IndexRange src_segments = bke::curves::per_curve_point_offsets_range(src_points,
                                                                               curve_i);
    const OffsetIndices<int> curve_offsets = all_point_offsets.slice(src_segments);

    interpolate_attribute_linear<T>(curve_offsets, src.slice(src_points), dst.slice(dst_points));
  });
}

static void subdivide_attribute_linear(const OffsetIndices<int> src_points_by_curve,
                                       const OffsetIndices<int> dst_points_by_curve,
                                       const IndexMask &selection,
                                       const Span<int> all_point_offsets,
                                       const GSpan src,
                                       GMutableSpan dst)
{
  bke::attribute_math::convert_to_static_type(dst.type(), [&](auto dummy) {
    using T = decltype(dummy);
    subdivide_attribute_linear(src_points_by_curve,
                               dst_points_by_curve,
                               selection,
                               all_point_offsets,
                               src.typed<T>(),
                               dst.typed<T>());
  });
}

static void subdivide_attribute_catmull_rom(const OffsetIndices<int> src_points_by_curve,
                                            const OffsetIndices<int> dst_points_by_curve,
                                            const IndexMask &selection,
                                            const Span<int> all_point_offsets,
                                            const Span<bool> cyclic,
                                            const GSpan src,
                                            GMutableSpan dst)
{
  selection.foreach_index(GrainSize(512), [&](const int curve_i) {
    const IndexRange src_points = src_points_by_curve[curve_i];
    const IndexRange src_segments = bke::curves::per_curve_point_offsets_range(src_points,
                                                                               curve_i);
    const IndexRange dst_points = dst_points_by_curve[curve_i];
    bke::curves::catmull_rom::interpolate_to_evaluated(src.slice(src_points),
                                                       cyclic[curve_i],
                                                       all_point_offsets.slice(src_segments),
                                                       dst.slice(dst_points));
  });
}

static HandleType aligned_or_free_handle_type(const HandleType type)
{
  switch (type) {
    case BEZIER_HANDLE_FREE:
      return BEZIER_HANDLE_FREE;
    case BEZIER_HANDLE_AUTO:
      return BEZIER_HANDLE_ALIGN;
    case BEZIER_HANDLE_VECTOR:
      return BEZIER_HANDLE_FREE;
    case BEZIER_HANDLE_ALIGN:
      return BEZIER_HANDLE_ALIGN;
  }
  BLI_assert_unreachable();
  return BEZIER_HANDLE_FREE;
}

static void subdivide_bezier_segment(const float3 &position_prev,
                                     const float3 &handle_prev,
                                     const float3 &handle_next,
                                     const float3 &position_next,
                                     const HandleType type_prev,
                                     const HandleType type_next,
                                     const IndexRange segment_points,
                                     const int dst_next_segment_start,
                                     MutableSpan<float3> dst_positions,
                                     MutableSpan<float3> dst_handles_l,
                                     MutableSpan<float3> dst_handles_r,
                                     MutableSpan<int8_t> dst_types_l,
                                     MutableSpan<int8_t> dst_types_r)
{
  if (bke::curves::bezier::segment_is_vector(type_prev, type_next)) {
    linear_interpolation(position_prev, position_next, dst_positions.slice(segment_points));
    /* All of the segment handles should be vector handles. */
    dst_types_r[segment_points.first()] = BEZIER_HANDLE_VECTOR;
    dst_types_l[dst_next_segment_start] = BEZIER_HANDLE_VECTOR;
    dst_types_l.slice(segment_points.drop_front(1)).fill(BEZIER_HANDLE_VECTOR);
    dst_types_r.slice(segment_points.drop_front(1)).fill(BEZIER_HANDLE_VECTOR);
  }
  else {
    /* The first point in the segment is always copied. */
    dst_positions[segment_points.first()] = position_prev;

    /* In order to generate a Bezier curve with the same shape as the input curve, apply the
     * De Casteljau algorithm iteratively for the provided number of cuts, constantly updating the
     * previous result point's right handle and the left handle at the end of the segment. */
    float3 segment_start = position_prev;
    float3 segment_handle_prev = handle_prev;
    float3 segment_handle_next = handle_next;
    const float3 segment_end = position_next;

    for (const int i : IndexRange(segment_points.size() - 1)) {
      const float parameter = 1.0f / (segment_points.size() - i);
      const int point_i = segment_points[i];
      bke::curves::bezier::Insertion insert = bke::curves::bezier::insert(
          segment_start, segment_handle_prev, segment_handle_next, segment_end, parameter);

      /* Copy relevant temporary data to the result. */
      dst_handles_r[point_i] = insert.handle_prev;
      dst_handles_l[point_i + 1] = insert.left_handle;
      dst_positions[point_i + 1] = insert.position;

      /* Update the segment to prepare it for the next subdivision. */
      segment_start = insert.position;
      segment_handle_prev = insert.right_handle;
      segment_handle_next = insert.handle_next;
    }

    /* Copy the handles for the last segment from the working variables. */
    dst_handles_r[segment_points.last()] = segment_handle_prev;
    dst_handles_l[dst_next_segment_start] = segment_handle_next;

    /* First and last handles at the ends of the segment are aligned if possible. */
    dst_types_r[segment_points.first()] = aligned_or_free_handle_type(type_prev);
    dst_types_l[dst_next_segment_start] = aligned_or_free_handle_type(type_next);

    /* Handles inside the segment are aligned. */
    dst_types_l.slice(segment_points.drop_front(1)).fill(BEZIER_HANDLE_ALIGN);
    dst_types_r.slice(segment_points.drop_front(1)).fill(BEZIER_HANDLE_ALIGN);
  }
}

static void subdivide_bezier_positions(const Span<float3> src_positions,
                                       const Span<int8_t> src_types_l,
                                       const Span<int8_t> src_types_r,
                                       const Span<float3> src_handles_l,
                                       const Span<float3> src_handles_r,
                                       const OffsetIndices<int> evaluated_offsets,
                                       const bool cyclic,
                                       MutableSpan<float3> dst_positions,
                                       MutableSpan<int8_t> dst_types_l,
                                       MutableSpan<int8_t> dst_types_r,
                                       MutableSpan<float3> dst_handles_l,
                                       MutableSpan<float3> dst_handles_r)
{
  threading::parallel_for(src_positions.index_range().drop_back(1), 512, [&](IndexRange range) {
    for (const int segment_i : range) {
      const IndexRange segment = evaluated_offsets[segment_i];
      subdivide_bezier_segment(src_positions[segment_i],
                               src_handles_r[segment_i],
                               src_handles_l[segment_i + 1],
                               src_positions[segment_i + 1],
                               HandleType(src_types_r[segment_i]),
                               HandleType(src_types_l[segment_i + 1]),
                               segment,
                               segment.one_after_last(),
                               dst_positions,
                               dst_handles_l,
                               dst_handles_r,
                               dst_types_l,
                               dst_types_r);
    }
  });

  if (cyclic) {
    const int last_index = src_positions.index_range().last();
    const IndexRange segment = evaluated_offsets[last_index];
    subdivide_bezier_segment(src_positions.last(),
                             src_handles_r.last(),
                             src_handles_l.first(),
                             src_positions.first(),
                             HandleType(src_types_r.last()),
                             HandleType(src_types_l.first()),
                             segment,
                             0,
                             dst_positions,
                             dst_handles_l,
                             dst_handles_r,
                             dst_types_l,
                             dst_types_r);
  }
  else {
    dst_positions.last() = src_positions.last();
    dst_types_l.first() = src_types_l.first();
    dst_types_r.last() = src_types_r.last();
    dst_handles_l.first() = src_handles_l.first();
    dst_handles_r.last() = src_handles_r.last();
  }

  /* TODO: It would be possible to avoid calling this for all segments besides vector segments. */
  bke::curves::bezier::calculate_auto_handles(
      cyclic, dst_types_l, dst_types_r, dst_positions, dst_handles_l, dst_handles_r);
}

static std::pair<int, int> compute_knot_insertions(const int8_t order,
                                                   const Span<int> span_offsets,
                                                   const Span<float> src_knots,
                                                   const Span<int> point_offset_data,
                                                   Vector<float, 32> &r_knot_inserts)
{
  r_knot_inserts.reserve(point_offset_data.last() - point_offset_data.first());

  int span_index_first_cut = 0;
  const IndexRange span_range = span_offsets.index_range();
  for (int index : span_range) {
    const int index_span = span_offsets[index];

    const int shift = order / 2;
    const int index_point = index_span - shift;
    const bool is_last_span = index == span_range.size() - 1;
    const int index_npoint = is_last_span ? index_point + 1 : span_offsets[index + 1] - shift;

    const int dst_points_num = point_offset_data[index_npoint] - point_offset_data[index_point];
    const int src_points_num = index_npoint - index_point;
    const int cuts_num = dst_points_num - src_points_num;

    /* Compute knot inserts. Note that inserts are done in the span interval (start, end),
     * avoiding inserts at the control points!
     */
    const float knot_start = src_knots[index_span];
    const float knot_end = src_knots[index_span + 1];
    const float delta_step = (knot_end - knot_start) / (cuts_num + 1);
    float knot_insert = knot_start + delta_step;
    for (int i = 0; i < cuts_num; i++) {
      r_knot_inserts.append(knot_insert);
      knot_insert += delta_step;
    }

    if (cuts_num == 0 && index == span_index_first_cut) {
      span_index_first_cut++;
    }
  }
  return {span_offsets[span_index_first_cut], span_offsets.last()};
}

static void subdivide_nurbs_curve(const int8_t order,
                                  const Span<float> src_knots,
                                  const Span<float3> src_points,
                                  const Span<float> src_weights,
                                  const std::pair<int, int> span_range,
                                  const Span<float> knot_inserts,
                                  MutableSpan<float> dst_knots,
                                  MutableSpan<float3> dst_points,
                                  MutableSpan<float> dst_weights)
{
  if (knot_inserts.size() == 0) {
    array_utils::copy(src_knots, dst_knots);
    array_utils::copy(src_points, dst_points);
    array_utils::copy(src_weights, dst_weights);
  }
  else if (src_weights.size() > 0) {
    bke::curves::nurbs::knot_refine_rational(order,
                                             span_range.first,
                                             span_range.second,
                                             knot_inserts,
                                             src_knots,
                                             src_points,
                                             src_weights,
                                             dst_knots,
                                             dst_points,
                                             dst_weights);
  }
  else {
    bke::curves::nurbs::knot_refine(
        order, span_range.first, span_range.second, knot_inserts, src_knots, dst_knots);
    bke::curves::nurbs::knot_refine_attribute<float3>(order,
                                                      span_range.first,
                                                      span_range.second,
                                                      knot_inserts,
                                                      src_knots,
                                                      src_points,
                                                      dst_knots,
                                                      dst_points);
  }
}

bke::CurvesGeometry subdivide_curves(const bke::CurvesGeometry &src_curves,
                                     const IndexMask &selection,
                                     const VArray<int> &cuts,
                                     const bke::AttributeFilter &attribute_filter)
{
  if (src_curves.is_empty()) {
    return src_curves;
  }

  const OffsetIndices src_points_by_curve = src_curves.points_by_curve();
  /* Cyclic is accessed a lot, it's probably worth it to make sure it's a span. */
  const VArraySpan<bool> cyclic{src_curves.cyclic()};

  /* Avoid repeating edge cases by removing invalid nurbs from selection. */
  const bool has_nurbs = src_curves.has_curve_with_type(CURVE_TYPE_NURBS);
  const VArraySpan<int8_t> src_order = src_curves.nurbs_orders();
  const VArraySpan<int8_t> src_knot_mode = src_curves.nurbs_knots_modes();

  IndexMaskMemory reduced_selection_mem;
  IndexMask reduced_selection;
  if (has_nurbs) {
    reduced_selection = IndexMask::from_predicate(
        selection, GrainSize(1024), reduced_selection_mem, [&](const int64_t curve_index) {
          /* TODO: Support Cyclic Custom NURBS as well... */
          return !cyclic[curve_index] && bke::curves::nurbs::check_valid_num_and_order(
                                             src_points_by_curve[curve_index].size(),
                                             src_order[curve_index],
                                             cyclic[curve_index],
                                             KnotsMode(src_knot_mode[curve_index]));
        });
  }
  const IndexMask &subdiv_mask = has_nurbs ? reduced_selection : selection;

  IndexMaskMemory memory;
  const IndexMask unselected = subdiv_mask.complement(src_curves.curves_range(), memory);

  bke::CurvesGeometry dst_curves = bke::curves::copy_only_curve_domain(src_curves);
  /* Copy vertex groups from source curves to allow copying vertex group attributes. */
  BKE_defgroup_copy_list(&dst_curves.vertex_group_names, &src_curves.vertex_group_names);

  /* For each point, this contains the point offset in the corresponding result curve,
   * starting at zero. For example for two curves with four points each, the values might
   * look like this:
   *
   * |                     | Curve 0           | Curve 1            |
   * | ------------------- |---|---|---|---|---|---|---|---|---|----|
   * | Cuts                | 0 | 3 | 0 | 0 | - | 2 | 0 | 0 | 4 | -  |
   * | New Point Count     | 1 | 4 | 1 | 1 | - | 3 | 1 | 1 | 5 | -  |
   * | Accumulated Offsets | 0 | 1 | 5 | 6 | 7 | 0 | 3 | 4 | 5 | 10 |
   *
   * Storing the leading zero is unnecessary but makes the array a bit simpler to use by avoiding
   * a check for the first segment, and because some existing utilities also use leading zeros. */
  Array<int> all_point_offset_data(src_curves.points_num() + src_curves.curves_num());
#ifndef NDEBUG
  all_point_offset_data.fill(-1);
#endif
  calculate_result_offsets(src_curves,
                           subdiv_mask,
                           unselected,
                           cuts,
                           cyclic,
                           has_nurbs,
                           dst_curves.offsets_for_write(),
                           all_point_offset_data);
  const OffsetIndices dst_points_by_curve = dst_curves.points_by_curve();

  const Span<int> all_point_offsets(all_point_offset_data);

  dst_curves.resize(dst_curves.offsets().last(), dst_curves.curves_num());

  const bke::AttributeAccessor src_attributes = src_curves.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();

  Vector<bke::AttributeTransferData> attributes_to_transfer =
      bke::retrieve_attributes_for_transfer(
          src_attributes, dst_attributes, {bke::AttrDomain::Point}, attribute_filter);

  auto subdivide_catmull_rom = [&](const IndexMask &selection) {
    for (auto &attribute : attributes_to_transfer) {
      subdivide_attribute_catmull_rom(src_points_by_curve,
                                      dst_points_by_curve,
                                      selection,
                                      all_point_offsets,
                                      cyclic,
                                      attribute.src,
                                      attribute.dst.span);
    }
  };

  auto subdivide_poly = [&](const IndexMask &selection) {
    for (auto &attribute : attributes_to_transfer) {
      subdivide_attribute_linear(src_points_by_curve,
                                 dst_points_by_curve,
                                 selection,
                                 all_point_offsets,
                                 attribute.src,
                                 attribute.dst.span);
    }
  };

  auto subdivide_bezier = [&](const IndexMask &selection) {
    const Span<float3> src_positions = src_curves.positions();
    const VArraySpan<int8_t> src_types_l{src_curves.handle_types_left()};
    const VArraySpan<int8_t> src_types_r{src_curves.handle_types_right()};
    const Span<float3> src_handles_l = *src_curves.handle_positions_left();
    const Span<float3> src_handles_r = *src_curves.handle_positions_right();

    MutableSpan<float3> dst_positions = dst_curves.positions_for_write();
    MutableSpan<int8_t> dst_types_l = dst_curves.handle_types_left_for_write();
    MutableSpan<int8_t> dst_types_r = dst_curves.handle_types_right_for_write();
    MutableSpan<float3> dst_handles_l = dst_curves.handle_positions_left_for_write();
    MutableSpan<float3> dst_handles_r = dst_curves.handle_positions_right_for_write();
    const OffsetIndices<int> dst_points_by_curve = dst_curves.points_by_curve();

    selection.foreach_index(GrainSize(512), [&](const int curve_i) {
      const IndexRange src_points = src_points_by_curve[curve_i];
      const IndexRange src_segments = bke::curves::per_curve_point_offsets_range(src_points,
                                                                                 curve_i);
      const IndexRange dst_points = dst_points_by_curve[curve_i];
      subdivide_bezier_positions(src_positions.slice(src_points),
                                 src_types_l.slice(src_points),
                                 src_types_r.slice(src_points),
                                 src_handles_l.slice(src_points),
                                 src_handles_r.slice(src_points),
                                 all_point_offsets.slice(src_segments),
                                 cyclic[curve_i],
                                 dst_positions.slice(dst_points),
                                 dst_types_l.slice(dst_points),
                                 dst_types_r.slice(dst_points),
                                 dst_handles_l.slice(dst_points),
                                 dst_handles_r.slice(dst_points));
    });
  };

  auto subdivide_nurbs = [&](const IndexMask &selection) {
    const Span<float3> src_positions = src_curves.positions();
    const std::optional<Span<float>> src_weights = src_curves.nurbs_weights();

    const Span<float> src_custom_knots = src_curves.nurbs_custom_knots();
    const OffsetIndices<int> src_custom_knots_by_curve = src_curves.nurbs_custom_knots_by_curve();

    MutableSpan<float3> dst_positions = dst_curves.positions_for_write();
    MutableSpan<int8_t> dst_order = dst_curves.nurbs_orders_for_write();
    MutableSpan<int8_t> dst_knot_mode = dst_curves.nurbs_knots_modes_for_write();
    MutableSpan<float> dst_weights = src_weights.has_value() ?
                                         dst_curves.nurbs_weights_for_write() :
                                         MutableSpan<float>{};

    dst_order.copy_from(dst_order);

    /* Make knots custom! */
    dst_knot_mode.fill(NURBS_KNOT_MODE_CUSTOM);
    dst_curves.nurbs_custom_knots_update_size();
    const OffsetIndices<int> dst_custom_knots_by_curve = dst_curves.nurbs_custom_knots_by_curve();
    MutableSpan<float> dst_custom_knots = dst_curves.nurbs_custom_knots_for_write();

    Vector<std::pair<int, int>> span_ranges(selection.size());
    Vector<Vector<float, 32>> knots(selection.size());
    Vector<Vector<float, 32>> knot_inserts(selection.size());

    selection.foreach_segment(
        GrainSize(512), [&](const IndexMaskSegment segment, const int64_t mask_offset) {
          for (const int curve_index : segment) {
            const IndexRange src_points = src_points_by_curve[curve_index];
            const IndexRange dst_points = dst_points_by_curve[curve_index];

            const bool is_cyclic = cyclic[curve_index];
            const int8_t order = src_order[curve_index];
            const KnotsMode mode = KnotsMode(src_knot_mode[curve_index]);

            const IndexRange src_segments = bke::curves::per_curve_point_offsets_range(
                src_points, curve_index);
            const Span<int> all_point_offsets = all_point_offset_data.as_span().slice(
                src_segments);

            const int knots_num = bke::curves::nurbs::knots_num(
                src_points.size(), order, is_cyclic);
            knots[mask_offset].reinitialize(knots_num);
            bke::curves::nurbs::load_curve_knots(mode,
                                                 src_points.size(),
                                                 order,
                                                 is_cyclic,
                                                 src_custom_knots_by_curve[curve_index],
                                                 src_custom_knots,
                                                 knots[mask_offset]);

            Vector<int> span_offsets;
            bke::curves::nurbs::find_spans(
                src_points.size(), order, is_cyclic, knots[mask_offset], span_offsets);

            Span<float> src_weight_span = src_weights.has_value() ?
                                              src_weights.value().slice(src_points) :
                                              Span<float>{};
            MutableSpan<float> dst_weight_span = src_weights.has_value() ?
                                                     dst_weights.slice(dst_points) :
                                                     MutableSpan<float>{};
            span_ranges[mask_offset] = compute_knot_insertions(order,
                                                               span_offsets,
                                                               knots[mask_offset],
                                                               all_point_offsets,
                                                               knot_inserts[mask_offset]);
            subdivide_nurbs_curve(order,
                                  knots[mask_offset],
                                  src_positions.slice(src_points),
                                  src_weight_span,
                                  span_ranges[mask_offset],
                                  knot_inserts[mask_offset],
                                  dst_custom_knots.slice(dst_custom_knots_by_curve[curve_index]),
                                  dst_positions.slice(dst_points),
                                  dst_weight_span);
          }
        });

    /* Filter out positions and handles that are already interpolated. */
    const Set<StringRef> attributes_to_skip = {"position",
                                               "handle_type_left",
                                               "handle_type_right",
                                               "handle_right",
                                               "handle_left",
                                               "nurbs_weight"};
    for (auto &attribute : attributes_to_transfer) {
      if (attributes_to_skip.contains(attribute.name)) {
        continue;
      }
      const GSpan src = attribute.src;
      GMutableSpan dst = attribute.dst.span;

      bke::attribute_math::convert_to_static_type(dst.type(), [&](auto dummy) {
        using T = decltype(dummy);
        const Span<T> src_typed = src.typed<T>();
        MutableSpan<T> dst_typed = dst.typed<T>();
        selection.foreach_index(
            GrainSize(512), [&](const int64_t curve_index, const int64_t mask_offset) {
              bke::curves::nurbs::knot_refine_attribute<T>(
                  src_order[curve_index],
                  span_ranges[mask_offset].first,
                  span_ranges[mask_offset].second,
                  knot_inserts[mask_offset],
                  knots[mask_offset],
                  src_typed,
                  dst_custom_knots.slice(dst_custom_knots_by_curve[curve_index]),
                  dst_typed);
            });
      });
    }
  };

  bke::curves::foreach_curve_by_type(src_curves.curve_types(),
                                     src_curves.curve_type_counts(),
                                     subdiv_mask,
                                     subdivide_catmull_rom,
                                     subdivide_poly,
                                     subdivide_bezier,
                                     subdivide_nurbs);

  for (auto &attribute : attributes_to_transfer) {
    array_utils::copy_group_to_group(
        src_points_by_curve, dst_points_by_curve, unselected, attribute.src, attribute.dst.span);
    attribute.dst.finish();
  }

  bke::curves::nurbs::copy_custom_knots(src_curves, subdiv_mask, dst_curves);
  return dst_curves;
}

}  // namespace blender::geometry
