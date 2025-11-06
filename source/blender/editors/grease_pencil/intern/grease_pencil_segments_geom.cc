/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include <algorithm>
#include <functional>

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_lasso_2d.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_sort.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_curves.hh"

#include "ED_grease_pencil.hh"
#include "ED_view3d.hh"

namespace blender::ed::greasepencil::trim {

enum Side : uint8_t { Start = 0, End = 1 };
enum Distance : uint8_t { Min = 0, Max = 1 };

/* When looking for intersections, we need a little padding, otherwise we could miss curves
 * that intersect for the eye, but not in hard numbers. */
static constexpr int BBOX_PADDING = 2;

/**
 * Structure describing a curve segment (a point range in a curve) with end intersection points.
 * A Segment can go past the end of the source curve and loop back to the start.
 */
class Segment {
 public:
  /* Curve index. */
  int curve = -1;

  /* The start and end of the original curve is stored, because this segment may go past then end
   * and have to loop. */
  IndexRange src_points;

  /* Point range of the segment: starting point and end point. Matches the point offsets
   * in a CurvesGeometry. */
  int points[2] = {-1, -1};

  /* The normalized distance where the trim segment is intersected by another curve.
   * For the outer ends of the trim segment the intersection distance is given between:
   * - [start point] and [start point + 1]
   * - [end point] and [end point + 1]
   */
  float intersection_factor[2] = {0.0f, 0.0f};

  int intersection_index[2] = {-1, -1};

 public:
  constexpr Segment() = default;

  bool is_loop() const
  {
    /* If both intersection points are the same, then it's a full loop. */
    if (points[Side::Start] == points[Side::End]) {
      if (intersection_factor[Side::Start] == intersection_factor[Side::End]) {
        return true;
      }
    }

    return intersection_factor[Side::End] == 1.0f;
  }

  bool has_intersection(const Side side) const
  {
    return intersection_factor[side] != 0.0f && intersection_factor[side] != 1.0f;
  }

  int2 edge(const Side side) const
  {
    return int2(points[side], this->wrap_index(points[side] + 1));
  }

  int wrap_index(const int i) const
  {
    return math::mod_periodic(i - src_points.first(), src_points.size()) + src_points.first();
  }

  IndexRange point_range() const
  {
    if (this->is_loop()) {
      return src_points;
    }

    if (!this->has_intersection(Side::Start) && this->has_intersection(Side::End)) {
      return IndexRange::from_begin_end_inclusive(src_points.first(), points[Side::End]);
    }

    if (!this->has_intersection(Side::Start) && !this->has_intersection(Side::End)) {
      return src_points;
    }

    /* If both intersection points are on the same edge, there's ether no points between or
     * all of the points are. */
    if (points[Side::Start] == points[Side::End]) {
      if (intersection_factor[Side::Start] >= intersection_factor[Side::End]) {
        return src_points.shift(points[Side::Start] - src_points.first() + 1);
      }
      return IndexRange(0);
    }

    if (points[Side::Start] > points[Side::End]) {
      return IndexRange::from_begin_end_inclusive(points[Side::Start] + 1,
                                                  points[Side::End] + src_points.size());
    }

    return IndexRange::from_begin_end_inclusive(points[Side::Start] + 1, points[Side::End]);
  }

  int points_num() const
  {
    return this->point_range().size();
  }

  template<typename Fn> inline void foreach_point(Fn &&fn) const
  {
    const IndexRange point_range = this->point_range();

    for (const int64_t pos : point_range.index_range()) {
      const int i = this->wrap_index(point_range[pos]);

      if constexpr (std::is_invocable_r_v<void, Fn, int64_t, int64_t>) {
        fn(i, pos);
      }
      else {
        fn(i);
      }
    }
  }

  constexpr static Segment from_curve(const int curve_i,
                                      const IndexRange points,
                                      const bool cyclical)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.src_points = points;

    segment.points[Side::Start] = points.first();
    segment.points[Side::End] = points.last();

    segment.intersection_factor[Side::Start] = 0.0f;
    segment.intersection_factor[Side::End] = cyclical ? 1.0f : 0.0f;

    return segment;
  }

  static Segment from_intersections(const int curve_i,
                                    const IndexRange points,
                                    const std::optional<float> parameter_start,
                                    const std::optional<float> parameter_end,
                                    const std::optional<int> inter_index_start,
                                    const std::optional<int> inter_index_end)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.src_points = points;

    if (parameter_start) {
      segment.points[Side::Start] = int(math::floor(*parameter_start));
      segment.intersection_factor[Side::Start] = math::fract(*parameter_start);
    }
    else {
      segment.points[Side::Start] = points.first();
      segment.intersection_factor[Side::Start] = 0.0f;
    }

    if (inter_index_start) {
      segment.intersection_index[Side::Start] = *inter_index_start;
    }

    if (parameter_end) {
      segment.points[Side::End] = int(math::floor(*parameter_end));
      segment.intersection_factor[Side::End] = math::fract(*parameter_end);
    }
    else {
      segment.points[Side::End] = points.last();
      segment.intersection_factor[Side::End] = 0.0f;
    }

    if (inter_index_end) {
      segment.intersection_index[Side::End] = *inter_index_end;
    }

    return segment;
  }
};

/**
 * Get the intersection distance of two line segments a-b and c-d.
 * The intersection distance is defined as the normalized distance (0..1)
 * from point a to the intersection point of a-b and c-d.
 */
static float get_intersection_distance_of_segments(const float2 &co_a,
                                                   const float2 &co_b,
                                                   const float2 &co_c,
                                                   const float2 &co_d)
{
  /* Get intersection point. */
  const float a1 = co_b[1] - co_a[1];
  const float b1 = co_a[0] - co_b[0];
  const float c1 = a1 * co_a[0] + b1 * co_a[1];

  const float a2 = co_d[1] - co_c[1];
  const float b2 = co_c[0] - co_d[0];
  const float c2 = a2 * co_c[0] + b2 * co_c[1];

  const float det = (a1 * b2 - a2 * b1);
  if (det == 0.0f) {
    return 0.0f;
  }

  const float2 isect((b2 * c1 - b1 * c2) / det, (a1 * c2 - a2 * c1) / det);

  /* Get normalized distance from point a to intersection point. */
  const float length_ab = math::length(co_b - co_a);
  const float distance = math::safe_divide(math::length(isect - co_a), length_ab);

  /* Snap to the ends if very close. */
  if (math::abs(distance) < 0.0001f) {
    return 0.0f;
  }
  if (math::abs(distance - 1.0f) < 0.0001f) {
    return 1.0f;
  }

  return distance;
}

static void calculate_offsets_from_segments(const Span<Segment> segments,
                                            const OffsetIndices<int> segment_offsets,
                                            const Span<bool> cyclic,
                                            MutableSpan<int> offsets)
{
  int offset = 0;

  for (const int curve_i : segment_offsets.index_range()) {
    offsets[curve_i] = offset;

    const IndexRange segment_range = segment_offsets[curve_i];
    for (const int seg_i : segment_range) {
      const Segment &segment = segments[seg_i];

      if (segment.has_intersection(Side::Start) && !segment.is_loop()) {
        offset++;
      }
      offset += segment.points_num();
      if (seg_i == segment_range.last() && segment.has_intersection(Side::End) && !cyclic[curve_i])
      {
        offset++;
      }
    }
  }

  offsets.last() = offset;
}

static bke::CurvesGeometry create_curves_from_segments(const bke::CurvesGeometry &src,
                                                       const Span<Segment> segments,
                                                       const Span<bool> segment_reversed,
                                                       const Span<bool> cyclic,
                                                       const OffsetIndices<int> segment_offsets)
{
  Array<int> point_offsets(segment_offsets.size() + 1);
  calculate_offsets_from_segments(
      segments, segment_offsets, cyclic, point_offsets.as_mutable_span());

  const bke::AttributeAccessor src_attributes = src.attributes();

  const OffsetIndices<int> dst_points_by_curve = OffsetIndices<int>(point_offsets);

  if (dst_points_by_curve.total_size() == 0) {
    return bke::CurvesGeometry();
  }

  bke::CurvesGeometry dst_curves(dst_points_by_curve.total_size(), dst_points_by_curve.size());
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();

  dst_curves.offsets_for_write().copy_from(dst_points_by_curve.data());
  dst_curves.cyclic_for_write().copy_from(cyclic);

  Array<int> old_by_new_map(dst_points_by_curve.size());

  threading::parallel_for(dst_points_by_curve.index_range(), 4096, [&](const IndexRange points) {
    for (const int i : points) {
      const IndexRange segment_range = segment_offsets[i];
      old_by_new_map[i] = segments[segment_range.first()].curve;
    }
  });

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic"}),
                         old_by_new_map,
                         dst_attributes);

  /* Copy/Interpolate point attributes. */
  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes, dst_attributes, {bke::AttrDomain::Point}, {}))
  {
    bke::attribute_math::convert_to_static_type(attribute.dst.span.type(), [&](auto dummy) {
      using T = decltype(dummy);
      auto src_attr = attribute.src.typed<T>();
      auto dst_attr = attribute.dst.span.typed<T>();

      int i = 0;

      for (const int curve_i : segment_offsets.index_range()) {
        const IndexRange segment_range = segment_offsets[curve_i];
        for (const int seg_i : segment_range) {
          const Segment &segment = segments[seg_i];
          const bool reversed = segment_reversed[seg_i];

          if (reversed ? segment.has_intersection(Side::End) :
                         segment.has_intersection(Side::Start) && !segment.is_loop())
          {
            const float start_factor = reversed ? segment.intersection_factor[Side::End] :
                                                  segment.intersection_factor[Side::Start];
            const int2 start_edge = reversed ? segment.edge(Side::End) : segment.edge(Side::Start);
            dst_attr[i++] = bke::attribute_math::mix2<T>(
                start_factor, src_attr[start_edge.x], src_attr[start_edge.y]);
          }

          segment.foreach_point(
              [&](const int index, const int pos) { dst_attr[pos + i] = src_attr[index]; });

          if (reversed) {
            dst_attr.slice(IndexRange::from_begin_size(i, segment.points_num())).reverse();
          }

          i += segment.points_num();

          if (seg_i == segment_range.last() &&
              (reversed ? segment.has_intersection(Side::Start) :
                          segment.has_intersection(Side::End)) &&
              !cyclic[curve_i])
          {
            const float end_factor = reversed ? segment.intersection_factor[Side::Start] :
                                                segment.intersection_factor[Side::End];
            const int2 end_edge = reversed ? segment.edge(Side::Start) : segment.edge(Side::End);
            dst_attr[i++] = bke::attribute_math::mix2<T>(
                end_factor, src_attr[end_edge.x], src_attr[end_edge.y]);
          }
        }
      }
    });

    attribute.dst.finish();
  }

  return dst_curves;
}

struct IntersectionPoint {
  int point_i = -1;
  int point_j = -1;
  float factor_i = -1.0f;
  float factor_j = -1.0f;
  int curve_i = -1;
  int curve_j = -1;

  int segment_index_i[2] = {-1, -1};
  int segment_index_j[2] = {-1, -1};

  constexpr IntersectionPoint() = default;

  float parameter_for_curve(const int curve) const
  {
    BLI_assert(curve == curve_i || curve == curve_j);
    return curve == curve_i ? point_i + factor_i : point_j + factor_j;
  }
};

static IntersectionPoint create_intersection(const int point_i,
                                             const int point_j,
                                             const float factor_i,
                                             const float factor_j,
                                             const int curve_i,
                                             const int curve_j)
{
  IntersectionPoint inter_point;
  inter_point.point_i = point_i;
  inter_point.point_j = point_j;
  inter_point.factor_i = factor_i;
  inter_point.factor_j = factor_j;
  inter_point.curve_i = curve_i;
  inter_point.curve_j = curve_j;

  return inter_point;
}

static void find_intersections_between_curve_and_curves(const int curve_i,
                                                        const Span<float2> screen_space_positions,
                                                        const Span<rcti> screen_space_curve_bounds,
                                                        const OffsetIndices<int> points_by_curve,
                                                        const VArray<bool> &cyclic,
                                                        const IndexMask &visible_curves,
                                                        Array<Vector<int>> &r_inters_per_curves,
                                                        Vector<IntersectionPoint> &r_intersections)
{
  const bool cyclic_i = cyclic[curve_i];
  const IndexRange points_i = points_by_curve[curve_i];

  for (const int i : points_i.index_range().drop_back(cyclic_i ? 0 : 1)) {
    const int point_i1 = points_i[i];
    const int point_i2 = points_i[(i + 1) % points_i.size()];

    const float2 co_i1 = screen_space_positions[point_i1];
    const float2 co_i2 = screen_space_positions[point_i2];

    rcti bbox_i;
    BLI_rcti_init_minmax(&bbox_i);
    BLI_rcti_do_minmax_v(&bbox_i, int2(co_i1));
    BLI_rcti_do_minmax_v(&bbox_i, int2(co_i2));
    BLI_rcti_pad(&bbox_i, BBOX_PADDING, BBOX_PADDING);

    /* Add some padding to the line segment i1-i2, otherwise we could just miss an
     * intersection. */
    const float2 padding_i = math::normalize(co_i2 - co_i1);
    const float2 padded_i1 = co_i1 - padding_i;
    const float2 padded_i2 = co_i2 + padding_i;

    visible_curves.foreach_index([&](const int curve_j) {
      if (curve_i > curve_j) {
        return;
      }

      /* Bounding box check: skip curves that don't overlap segment i1-i2. */
      if (!BLI_rcti_isect(&bbox_i, &screen_space_curve_bounds[curve_j], nullptr)) {
        return;
      }

      const bool cyclic_j = cyclic[curve_j];
      const IndexRange points_j = points_by_curve[curve_j];

      for (const int j : points_j.index_range().drop_back(cyclic_j ? 0 : 1)) {
        const int point_j1 = points_j[j];
        const int point_j2 = points_j[(j + 1) % points_j.size()];

        /* Don't self check. */
        if (curve_i == curve_j && (point_i1 == point_j1 || point_i1 == point_j2 ||
                                   point_i2 == point_j1 || point_i2 == point_j2))
        {
          continue;
        }

        const float2 co_j1 = screen_space_positions[point_j1];
        const float2 co_j2 = screen_space_positions[point_j2];

        /* Skip when bounding boxes of i1-i2 and j1-j2 don't overlap. */
        rcti bbox_j;
        BLI_rcti_init_minmax(&bbox_j);
        BLI_rcti_do_minmax_v(&bbox_j, int2(co_j1));
        BLI_rcti_do_minmax_v(&bbox_j, int2(co_j2));
        BLI_rcti_pad(&bbox_j, BBOX_PADDING, BBOX_PADDING);
        if (!BLI_rcti_isect(&bbox_i, &bbox_j, nullptr)) {
          continue;
        }

        /* Add some padding to the line segment j1-j2, otherwise we could just miss an
         * intersection. */
        const float2 padding_j = math::normalize(co_j2 - co_j1);
        const float2 padded_j1 = co_j1 - padding_j;
        const float2 padded_j2 = co_j2 + padding_j;

        /* Check for intersection. */
        const auto isect = math::isect_seg_seg(padded_i1, padded_i2, padded_j1, padded_j2);
        if (ELEM(isect.kind, isect.LINE_LINE_CROSS, isect.LINE_LINE_EXACT)) {
          const float factor_i = get_intersection_distance_of_segments(co_i1, co_i2, co_j1, co_j2);
          const float factor_j = get_intersection_distance_of_segments(co_j1, co_j2, co_i1, co_i2);

          r_inters_per_curves[curve_i].append(r_intersections.size());
          r_inters_per_curves[curve_j].append(r_intersections.size());
          r_intersections.append(
              create_intersection(point_i1, point_j1, factor_i, factor_j, curve_i, curve_j));
        }
      }
    });
  }
}

/* TODO: This method of finding intersections is O(N^2) and should replaced with something faster.
 */
static void find_intersections_between_all_curves(const Span<float2> screen_space_positions,
                                                  const Span<rcti> screen_space_curve_bounds,
                                                  const OffsetIndices<int> points_by_curve,
                                                  const VArray<bool> &cyclic,
                                                  const IndexMask &visible_curves,
                                                  Array<Vector<int>> &r_inters_per_curves,
                                                  Vector<IntersectionPoint> &r_intersections)
{
  visible_curves.foreach_index([&](const int curve_i) {
    find_intersections_between_curve_and_curves(curve_i,
                                                screen_space_positions,
                                                screen_space_curve_bounds,
                                                points_by_curve,
                                                cyclic,
                                                visible_curves,
                                                r_inters_per_curves,
                                                r_intersections);
  });
}

static void store_segment_map_on_intersections(const Span<Segment> all_segments,
                                               MutableSpan<IntersectionPoint> intersections)
{
  for (const int seg_i : all_segments.index_range()) {
    const Segment &segment = all_segments[seg_i];
    const int curve_i = segment.curve;

    if (segment.has_intersection(Side::Start)) {
      IntersectionPoint &inter_start = intersections[segment.intersection_index[Side::Start]];
      if (curve_i == inter_start.curve_i) {
        inter_start.segment_index_i[Side::End] = seg_i;
      }
      else {
        inter_start.segment_index_j[Side::End] = seg_i;
      }
    }

    if (segment.has_intersection(Side::End)) {
      IntersectionPoint &inter_end = intersections[segment.intersection_index[Side::End]];
      if (curve_i == inter_end.curve_i) {
        inter_end.segment_index_i[Side::Start] = seg_i;
      }
      else {
        inter_end.segment_index_j[Side::Start] = seg_i;
      }
    }
  }
}

static void create_segments_from_intersections(const Span<Vector<int>> inters_per_curves,
                                               const OffsetIndices<int> points_by_curve,
                                               const Span<IntersectionPoint> &intersections,
                                               const VArray<bool> &cyclic,
                                               Vector<Segment> &all_segments,
                                               MutableSpan<IndexRange> all_segments_by_curve)
{
  for (const int curve_k : points_by_curve.index_range()) {
    const IndexRange points_k = points_by_curve[curve_k];
    const Span<int> inters = inters_per_curves[curve_k];

    const int start_size = all_segments.size();

    if (inters.size() == 0) {
      all_segments.append(Segment::from_curve(curve_k, points_k, cyclic[curve_k]));
      all_segments_by_curve[curve_k] = all_segments.index_range().drop_front(start_size);

      return;
    }

    Array<int> inter_sorted_ids = Array<int>(inters.size());
    array_utils::fill_index_range<int>(inter_sorted_ids);

    parallel_sort(inter_sorted_ids.begin(), inter_sorted_ids.end(), [&](int i1, int i2) {
      const IntersectionPoint &inter1 = intersections[inters[i1]];
      const IntersectionPoint &inter2 = intersections[inters[i2]];
      return inter1.parameter_for_curve(curve_k) < inter2.parameter_for_curve(curve_k);
    });

    if (cyclic[curve_k]) {
      const int int_p_1 = inters[inter_sorted_ids.first()];
      const int int_p_2 = inters[inter_sorted_ids.last()];

      const IntersectionPoint &inter_first = intersections[int_p_1];
      const IntersectionPoint &inter_last = intersections[int_p_2];

      all_segments.append(Segment::from_intersections(curve_k,
                                                      points_k,
                                                      inter_last.parameter_for_curve(curve_k),
                                                      inter_first.parameter_for_curve(curve_k),
                                                      int_p_2,
                                                      int_p_1));
    }
    else {
      const int int_p_1 = inters[inter_sorted_ids.first()];
      const IntersectionPoint &inter_first = intersections[int_p_1];

      all_segments.append(Segment::from_intersections(curve_k,
                                                      points_k,
                                                      std::nullopt,
                                                      inter_first.parameter_for_curve(curve_k),
                                                      std::nullopt,
                                                      int_p_1));
    }

    for (const int inter_id : inter_sorted_ids.index_range().drop_back(1)) {
      const int int_p_1 = inters[inter_sorted_ids[inter_id]];
      const int int_p_2 = inters[inter_sorted_ids[inter_id + 1]];

      const IntersectionPoint &inter_first = intersections[int_p_1];
      const IntersectionPoint &inter_last = intersections[int_p_2];

      if (inter_first.parameter_for_curve(curve_k) != inter_last.parameter_for_curve(curve_k)) {
        all_segments.append(Segment::from_intersections(curve_k,
                                                        points_k,
                                                        inter_first.parameter_for_curve(curve_k),
                                                        inter_last.parameter_for_curve(curve_k),
                                                        int_p_1,
                                                        int_p_2));
      }
    }

    if (!(cyclic[curve_k])) {
      const int int_p_2 = inters[inter_sorted_ids.last()];
      const IntersectionPoint &inter_last = intersections[int_p_2];

      all_segments.append(Segment::from_intersections(curve_k,
                                                      points_k,
                                                      inter_last.parameter_for_curve(curve_k),
                                                      std::nullopt,
                                                      int_p_2,
                                                      std::nullopt));
    }

    all_segments_by_curve[curve_k] = all_segments.index_range().drop_front(start_size);
  }
}

static bool check_and_join_segments(Segment &first, const Segment &second)
{
  if (first.curve != second.curve) {
    return false;
  }

  if (first.intersection_index[Side::End] == second.intersection_index[Side::Start] &&
      first.intersection_index[Side::End] != -1)
  {
    first.points[Side::End] = second.points[Side::End];
    first.intersection_factor[Side::End] = second.intersection_factor[Side::End];
    first.intersection_index[Side::End] = second.intersection_index[Side::End];

    return true;
  }
  if (first.intersection_index[Side::Start] == second.intersection_index[Side::End] &&
      first.intersection_index[Side::Start] != -1)
  {
    first.points[Side::Start] = second.points[Side::Start];
    first.intersection_factor[Side::Start] = second.intersection_factor[Side::Start];
    first.intersection_index[Side::Start] = second.intersection_index[Side::Start];

    return true;
  }

  return false;
}

static void cut_caps(bke::CurvesGeometry &dst,
                     const Span<Segment> segments,
                     const Span<bool> segment_reversed,
                     const Span<bool> cyclic,
                     const OffsetIndices<int> segment_offsets)
{
  bke::MutableAttributeAccessor dst_attributes = dst.attributes_for_write();

  bke::SpanAttributeWriter<int8_t> dst_start_caps =
      dst_attributes.lookup_or_add_for_write_span<int8_t>("start_cap", bke::AttrDomain::Curve);
  bke::SpanAttributeWriter<int8_t> dst_end_caps =
      dst_attributes.lookup_or_add_for_write_span<int8_t>("end_cap", bke::AttrDomain::Curve);

  for (const int curve_i : segment_offsets.index_range()) {
    /* If the curve is cyclic, don't cut it. */
    if (cyclic[curve_i]) {
      continue;
    }

    const IndexRange segment_range = segment_offsets[curve_i];

    const int segment_index_first = segment_range.first();
    const bool reversed_first = segment_reversed[segment_index_first];
    const Segment &segment_first = segments[segment_index_first];
    const Side direction_first = reversed_first ? Side::End : Side::Start;
    const int inter_index_first = segment_first.intersection_index[direction_first];

    const int segment_index_last = segment_range.last();
    const bool reversed_last = segment_reversed[segment_index_last];
    const Segment &segment_last = segments[segment_index_last];
    const Side direction_last = reversed_last ? Side::Start : Side::End;
    const int inter_index_last = segment_last.intersection_index[direction_last];

    /* Check if there is intersection and therefor the segment should be cut. */
    if (inter_index_first != -1) {
      dst_start_caps.span[curve_i] = GP_STROKE_CAP_TYPE_FLAT;
    }
    if (inter_index_last != -1) {
      dst_end_caps.span[curve_i] = GP_STROKE_CAP_TYPE_FLAT;
    }
  }

  dst_start_caps.finish();
  dst_end_caps.finish();
}

/* We store the side as sign, but because a segment with index zero is valid, we shift by one. */
using EncodedConnection = int;
static constexpr EncodedConnection SEGMENT_CONNECTION_NULL = 0;

static EncodedConnection encode_index_and_side(const int index, const Side side)
{
  return side == Side::Start ? index + 1 : -(index + 1);
}

static int decode_index(const EncodedConnection encoded)
{
  return math::abs(encoded) - 1;
}

static Side decode_side(const EncodedConnection encoded)
{
  return encoded < 0 ? Side::End : Side::Start;
}

/* Both the start and end of the segment are connected to two other segments*/
using SegmentConnections = VecBase<EncodedConnection, 2>;

static void create_connections_from_curves(const Span<IndexRange> segments_by_curve,
                                           const Span<bool> segments_to_keep,
                                           const VArray<bool> is_cyclic,
                                           MutableSpan<SegmentConnections> segment_connections)
{

  threading::parallel_for(segments_by_curve.index_range(), 4096, [&](const IndexRange curves) {
    for (const int curve_i : curves) {
      const IndexRange segment_range = segments_by_curve[curve_i];

      if (segment_range.size() == 1) {
        if (segments_to_keep[segment_range.first()]) {
          segment_connections[segment_range.first()][Side::Start] = SEGMENT_CONNECTION_NULL;
          segment_connections[segment_range.first()][Side::End] = SEGMENT_CONNECTION_NULL;
        }
        continue;
      }

      for (const int segment_i : segment_range.drop_back(1)) {
        if (!segments_to_keep[segment_i]) {
          continue;
        }

        if (segments_to_keep[segment_i + 1]) {
          segment_connections[segment_i][Side::End] = encode_index_and_side(segment_i + 1,
                                                                            Side::Start);
          segment_connections[segment_i + 1][Side::Start] = encode_index_and_side(segment_i,
                                                                                  Side::End);
        }
        else {
          segment_connections[segment_i][Side::End] = SEGMENT_CONNECTION_NULL;
        }
      }

      if (!segments_to_keep[segment_range.last()]) {
        continue;
      }

      if (!is_cyclic[curve_i]) {
        segment_connections[segment_range.first()][Side::Start] = SEGMENT_CONNECTION_NULL;
        segment_connections[segment_range.last()][Side::End] = SEGMENT_CONNECTION_NULL;
        continue;
      }

      if (segments_to_keep[segment_range.first()]) {
        segment_connections[segment_range.first()][Side::Start] = encode_index_and_side(
            segment_range.last(), Side::End);
        segment_connections[segment_range.last()][Side::End] = encode_index_and_side(
            segment_range.first(), Side::Start);
      }
      else {
        segment_connections[segment_range.last()][Side::End] = SEGMENT_CONNECTION_NULL;
      }
    }
  });
}

static void follow_segment_connections(const Span<Segment> all_segments,
                                       const Span<bool> segments_to_keep,
                                       const Span<SegmentConnections> segment_connections,
                                       Vector<Segment> &segments,
                                       Vector<int> &segment_offset_data,
                                       Vector<bool> &segment_reversed,
                                       Vector<bool> &cyclic)
{
  BLI_assert(all_segments.size() == segments_to_keep.size());
  BLI_assert(all_segments.size() == segment_connections.size());

  segment_offset_data.append(0);

  /* Follow each segment until it loops or ends. */
  Array<bool> processed_segments(all_segments.size(), false);
  int start_segment = 0;

  auto get_next_unprocessed_segment = [&]() {
    /* All segment before `start_segment` are guaranteed to be processed, so skip search them.
     * This optimization make the algorithm `O(N)` instead of `O(N^2)`.*/
    const int empty_num = start_segment;
    const int first_segment = processed_segments.as_span().drop_front(empty_num).first_index_try(
        false);

    if (first_segment == -1) {
      return -1;
    }
    return first_segment + empty_num;
  };

  /* Mark all segments that are not to keep as processed. */
  for (const int seg_i : all_segments.index_range()) {
    if (!segments_to_keep[seg_i]) {
      processed_segments[seg_i] = true;
    }
  }

  start_segment = get_next_unprocessed_segment();

  while (start_segment != -1) {
    bool current_backwards = false;
    int current_i = start_segment;

    bool curve_done = false;
    bool curve_closed = false;
    while (!curve_done) {
      if (processed_segments[current_i] == true) {
        BLI_assert_unreachable();
        break;
      }

      const Segment &current_segment = all_segments[current_i];
      processed_segments[current_i] = true;

      if (segments.size() == 0) {
        segments.append(current_segment);
        segment_reversed.append(current_backwards);
      }
      /* Check if the last segment can be joined with this one. */
      else if (!check_and_join_segments(segments.last(), current_segment)) {
        segments.append(current_segment);
        segment_reversed.append(current_backwards);
      }

      const EncodedConnection next_encoded =
          segment_connections[current_i][current_backwards ? Side::Start : Side::End];

      const int next_segment = decode_index(next_encoded);
      const Side next_side = decode_side(next_encoded);

      if (next_encoded == SEGMENT_CONNECTION_NULL) {
        curve_done = true;
        curve_closed = false;
        break;
      }

      if (next_segment == start_segment) {
        curve_done = true;
        curve_closed = true;

        BLI_assert(next_side == Side::Start);

        /* Check if the last segment in this curve can be joined to the first one in this curve. */
        if ((!segments.index_range().is_empty()) &&
            segment_offset_data.last() != segments.index_range().last())
        {
          if (check_and_join_segments(segments[segment_offset_data.last()], segments.last())) {
            segments.remove_last();
            segment_reversed.remove_last();
          }
        }

        break;
      }

      current_i = next_segment;
      current_backwards = next_side == Side::End;
    }
    segment_offset_data.append(segments.size());
    cyclic.append(curve_closed);

    /* Get the next unprocessed segment. */
    start_segment = get_next_unprocessed_segment();
  }
}

static bool check_line_segment_lasso_intersection(const int2 pos_a,
                                                  const int2 pos_b,
                                                  const Span<int2> mcoords)
{
  rcti bbox_ab;
  BLI_rcti_init_minmax(&bbox_ab);
  BLI_rcti_do_minmax_v(&bbox_ab, pos_a);
  BLI_rcti_do_minmax_v(&bbox_ab, pos_b);
  BLI_rcti_pad(&bbox_ab, BBOX_PADDING, BBOX_PADDING);

  /* Check the lasso bounding box first as an optimization. */
  if (BLI_rcti_isect_segment(&bbox_ab, pos_a, pos_b) &&
      BLI_lasso_is_edge_inside(mcoords, pos_a.x, pos_a.y, pos_b.x, pos_b.y, IS_CLIPPED))
  {
    return true;
  }
  return false;
}

static void check_segments_in_lasso(const Span<float2> screen_space_positions,
                                    const Span<rcti> screen_space_curve_bounds,
                                    const Span<int2> mcoords,
                                    const Span<Segment> all_segments,
                                    const IndexMask &editable_curves,
                                    const Span<IndexRange> segments_by_curve,
                                    MutableSpan<bool> segments_to_keep)
{
  rcti bbox_lasso;
  BLI_lasso_boundbox(&bbox_lasso, mcoords);

  editable_curves.foreach_index(GrainSize(128), [&](const int curve_i) {
    /* To speed things up: do a bounding box check on the curve and the lasso area. */
    if (!BLI_rcti_isect(&bbox_lasso, &screen_space_curve_bounds[curve_i], nullptr)) {
      return;
    }

    const IndexRange &segment_range = segments_by_curve[curve_i];
    for (const int segment_i : segment_range) {
      const Segment &segment = all_segments[segment_i];

      const IndexRange point_range = segment.point_range();

      if (point_range.is_empty()) {
        const float start_factor = segment.intersection_factor[Side::Start];
        const int2 start_edge = segment.edge(Side::Start);
        const float end_factor = segment.intersection_factor[Side::End];
        const int2 end_edge = segment.edge(Side::End);
        const float2 pos_1 = math::interpolate(screen_space_positions[start_edge.x],
                                               screen_space_positions[start_edge.y],
                                               start_factor);
        const float2 pos_2 = math::interpolate(
            screen_space_positions[end_edge.x], screen_space_positions[end_edge.y], end_factor);

        if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
          segments_to_keep[segment_i] = false;
        }

        continue;
      }

      for (const int64_t i : point_range.drop_back(1)) {
        const int point_i1 = segment.wrap_index(i);
        const int point_i2 = segment.wrap_index(i + 1);

        const float2 pos_1 = screen_space_positions[point_i1];
        const float2 pos_2 = screen_space_positions[point_i2];

        if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
          segments_to_keep[segment_i] = false;
          continue;
        }
      }

      if (segment_range.size() == 1 && segment.is_loop()) {
        const float2 pos_1 = screen_space_positions[segment.wrap_index(point_range.first())];
        const float2 pos_2 = screen_space_positions[segment.wrap_index(point_range.last())];

        if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
          segments_to_keep[segment_i] = false;
          continue;
        }
      }
      else {
        if (segment.has_intersection(Side::Start)) {
          const float start_factor = segment.intersection_factor[Side::Start];
          const int2 start_edge = segment.edge(Side::Start);
          const float2 pos_1 = math::interpolate(screen_space_positions[start_edge.x],
                                                 screen_space_positions[start_edge.y],
                                                 start_factor);
          const float2 pos_2 = screen_space_positions[segment.wrap_index(point_range.first())];

          if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
            segments_to_keep[segment_i] = false;
            continue;
          }
        }

        if (segment.has_intersection(Side::End)) {
          const float end_factor = segment.intersection_factor[Side::End];
          const int2 end_edge = segment.edge(Side::End);
          const float2 pos_1 = screen_space_positions[segment.wrap_index(point_range.last())];
          const float2 pos_2 = math::interpolate(
              screen_space_positions[end_edge.x], screen_space_positions[end_edge.y], end_factor);

          if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
            segments_to_keep[segment_i] = false;
            continue;
          }
        }
      }
    }
  });
}

bke::CurvesGeometry trim_curve_segments(const bke::CurvesGeometry &src,
                                        const Span<float2> screen_space_positions,
                                        const Span<rcti> screen_space_curve_bounds,
                                        const Span<int2> mcoords,
                                        const IndexMask &editable_curves,
                                        const IndexMask &visible_curves,
                                        const bool keep_caps)
{
  const OffsetIndices<int> src_points_by_curve = src.points_by_curve();
  const VArray<bool> is_cyclic = src.cyclic();

  if (src.is_empty()) {
    return src;
  }

  Vector<IntersectionPoint> intersections;
  Array<IndexRange> segments_by_curve(src_points_by_curve.size());
  Vector<Segment> all_segments;

  Array<Vector<int>> inters_per_curves(src_points_by_curve.size());
  find_intersections_between_all_curves(screen_space_positions,
                                        screen_space_curve_bounds,
                                        src_points_by_curve,
                                        is_cyclic,
                                        visible_curves,
                                        inters_per_curves,
                                        intersections);
  create_segments_from_intersections(inters_per_curves,
                                     src_points_by_curve,
                                     intersections,
                                     is_cyclic,
                                     all_segments,
                                     segments_by_curve);
  store_segment_map_on_intersections(all_segments, intersections);

  Array<bool> segments_to_keep(all_segments.size(), true);
  check_segments_in_lasso(screen_space_positions,
                          screen_space_curve_bounds,
                          mcoords,
                          all_segments,
                          editable_curves,
                          segments_by_curve,
                          segments_to_keep.as_mutable_span());

  Array<SegmentConnections> segment_connections(all_segments.size(),
                                                SegmentConnections(SEGMENT_CONNECTION_NULL));
  create_connections_from_curves(
      segments_by_curve, segments_to_keep, is_cyclic, segment_connections.as_mutable_span());

  Vector<Segment> segments;
  Vector<int> segment_offset_data;
  Vector<bool> segment_reversed;
  Vector<bool> cyclic;
  follow_segment_connections(all_segments,
                             segments_to_keep,
                             segment_connections,
                             segments,
                             segment_offset_data,
                             segment_reversed,
                             cyclic);
  const OffsetIndices<int> segment_offsets = OffsetIndices<int>(segment_offset_data);

  bke::CurvesGeometry dst = create_curves_from_segments(
      src, segments, segment_reversed, cyclic, segment_offsets);

  if (!keep_caps) {
    cut_caps(dst, segments, segment_reversed, cyclic, segment_offsets);
  }

  return dst;
}

bke::CurvesGeometry trim_curve_segment_ends(const bke::CurvesGeometry &src,
                                            const Span<float2> screen_space_positions,
                                            const Span<rcti> screen_space_curve_bounds,
                                            const IndexMask &editable_curves,
                                            const IndexMask &visible_curves,
                                            const bool keep_caps)
{
  const OffsetIndices<int> src_points_by_curve = src.points_by_curve();
  const VArray<bool> is_cyclic = src.cyclic();

  Vector<IntersectionPoint> intersections;
  Array<IndexRange> segments_by_curve(src_points_by_curve.size());
  Vector<Segment> all_segments;

  Array<Vector<int>> inters_per_curves(src_points_by_curve.size());
  find_intersections_between_all_curves(screen_space_positions,
                                        screen_space_curve_bounds,
                                        src_points_by_curve,
                                        is_cyclic,
                                        visible_curves,
                                        inters_per_curves,
                                        intersections);
  create_segments_from_intersections(inters_per_curves,
                                     src_points_by_curve,
                                     intersections,
                                     is_cyclic,
                                     all_segments,
                                     segments_by_curve);
  store_segment_map_on_intersections(all_segments, intersections);

  Array<bool> segments_to_keep(all_segments.size(), true);
  editable_curves.foreach_index(GrainSize(128), [&](const int curve_i) {
    const IndexRange segment_range = segments_by_curve[curve_i];

    if (segment_range.size() > 2) {
      segments_to_keep[segment_range.first()] = false;
      segments_to_keep[segment_range.last()] = false;
    }
  });

  Array<SegmentConnections> segment_connections(all_segments.size(),
                                                SegmentConnections(SEGMENT_CONNECTION_NULL));
  create_connections_from_curves(
      segments_by_curve, segments_to_keep, is_cyclic, segment_connections.as_mutable_span());

  Vector<Segment> segments;
  Vector<int> segment_offset_data;
  Vector<bool> segment_reversed;
  Vector<bool> cyclic;
  follow_segment_connections(all_segments,
                             segments_to_keep,
                             segment_connections,
                             segments,
                             segment_offset_data,
                             segment_reversed,
                             cyclic);
  const OffsetIndices<int> segment_offsets = OffsetIndices<int>(segment_offset_data);

  bke::CurvesGeometry dst = create_curves_from_segments(
      src, segments, segment_reversed, cyclic, segment_offsets);

  if (!keep_caps) {
    cut_caps(dst, segments, segment_reversed, cyclic, segment_offsets);
  }

  return dst;
}

}  // namespace blender::ed::greasepencil::trim
