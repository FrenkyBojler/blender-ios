/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_deform.hh"

#include "BLI_array_utils.hh"

#include "ED_curves.hh"

#include "nurbs_intern.hh"

namespace blender::ed::curves::nurbs {

static int count_knot_multiplicity_right(const float knot,
                                         const Span<float> knots,
                                         const int begin_from)
{
  int count = 0;
  for (const float current_knot : knots.drop_front(begin_from)) {
    if (current_knot != knot) {
      break;
    }
    count++;
  }
  return count;
}

static int count_knot_multiplicity_left(const float knot,
                                        const Span<float> knots,
                                        const int begin_from)
{
  int count = 0;
  for (int i = begin_from; i >= 0 && knots[i] == knot; i--) {
    count++;
  }
  return count;
}

void find_span_mult(
    const float knot, const Span<float> knots, const int order, int &r_span, int &r_mult)
{
  if (knot == knots.last(order - 1)) {
    r_span = knots.size() - order;
    r_mult = count_knot_multiplicity_left(knot, knots, r_span) +
             count_knot_multiplicity_right(knot, knots, r_span + 1);
    return;
  }
  int low = order - 1;
  int high = knots.size() - order;
  while (low <= high) {
    const int mid = low + (high - low + 1) / 2;
    if (knot < knots[mid]) {
      high = mid - 1;
    }
    else if (knot >= knots[mid + 1]) {
      low = mid;
    }
    else {
      r_span = mid;
      r_mult = count_knot_multiplicity_left(knot, knots, mid);
      return;
    }
  }
  r_span = -1;
  r_mult = 0;
  return;
}

WeightMatrix calc_knot_insertion_weights(const Span<float> knots,
                                         const int points_num,
                                         const int8_t order,
                                         const float knot,
                                         const int knot_span,
                                         const int mult,
                                         const int repeat)
{
  BLI_assert(repeat > 0);
  BLI_assert(mult + repeat < order);
  const int degree = order - 1;
  const int altered_point_num = degree - mult + repeat - 1;
  const int points_num_after = points_num + repeat;
  const IndexRange points_to_replace = IndexRange::from_begin_size(knot_span - degree + 1,
                                                                   altered_point_num - repeat);
  const IndexRange altered_points_range = IndexRange::from_begin_size(
      points_to_replace.start(), points_to_replace.size() + repeat);

  Vector<WeightTriplet> tris;
  tris.reserve(points_num + altered_points_range.size() * order);

  /* Set 1.0f for copied points. */
  for (const int i : IndexRange(points_to_replace.start())) {
    tris.append(WeightTriplet(i, i, 1.0f));
  }
  for (const int i : IndexRange::from_begin_end(points_to_replace.one_after_last(), points_num)) {
    tris.append(WeightTriplet(i + repeat, i, 1.0f));
  }

  Array<float> point_weights_buffer(order * order, 0.0f);
  MutableSpan<float> point_weights = point_weights_buffer.as_mutable_span();
  for (const int i : IndexRange(order)) {
    point_weights[i * order + i] = 1.0f;
  }

  for (const int r : IndexRange::from_begin_size(1, repeat)) {
    const int leg = knot_span - degree + r;
    for (const int i : IndexRange(order - r - mult)) {
      const float alpha = (knot - knots[leg + i]) / (knots[i + knot_span + 1] - knots[leg + i]);
      const MutableSpan<float> q_i_weights = point_weights.slice(i * order, order);
      const Span<float> q_i_1_weights = point_weights.slice((i + 1) * order, order);
      for (const int point : IndexRange(order)) {
        q_i_weights[point] = alpha * q_i_1_weights[point] + (1.0f - alpha) * q_i_weights[point];
      }
    }
    for (const int term_point : IndexRange(order)) {
      const int src_point = (knot_span - order + 1 + term_point) % points_num;
      const int dst_point_left = altered_points_range[r - 1];
      const int dst_point_right = altered_points_range.last(r - 1);
      tris.append(WeightTriplet(dst_point_left, src_point, point_weights[term_point]));
      tris.append(WeightTriplet(
          dst_point_right, src_point, point_weights[(degree - r - mult) * order + term_point]));
    }
  }

  for (const int j : IndexRange::from_begin_size(1, std::max(degree - mult - repeat - 1, 0))) {
    for (const int term_point : IndexRange(order)) {
      const int src_point = (knot_span - order + 1 + term_point) % points_num;
      tris.append(WeightTriplet(
          altered_points_range[j], src_point, point_weights[j * order + term_point]));
    }
  }
  WeightMatrix m(points_num_after, points_num);
  m.setFromTriplets(tris.begin(), tris.end());
  m.makeCompressed();
  return m;
}

Span<float> prepare_curve_weights(const Span<float> all_weights,
                                  const IndexRange curve_points,
                                  Array<float> &weights_buffer)
{
  if (!all_weights.is_empty()) {
    return all_weights.slice(curve_points);
  }
  weights_buffer.reinitialize(curve_points.size());
  weights_buffer.fill(1.0f);
  return weights_buffer;
}

IndexMask selection_from_modified(const WeightMatrix &point_weights, IndexMaskMemory &memory)
{
  Array<bool> modified_points(point_weights.rows(), false);
  threading::parallel_for(IndexRange(point_weights.rows()), 512, [&](IndexRange rows) {
    for (const int row : rows) {
      WeightMatrix::InnerIterator it(point_weights, row);
      const float value = it.value();
      modified_points[row] = (value != 1.0f) || (++it);
    }
  });
  return IndexMask::from_bools(modified_points, memory);
}

static void select_curve_points_modified_by_weights(const WeightMatrix &point_weights,
                                                    const int curve,
                                                    bke::CurvesGeometry &curves)
{
  foreach_selection_attribute_writer(
      curves, bke::AttrDomain::Point, [&](bke::GSpanAttributeWriter &selection) {
        fill_selection_false(selection.span);
      });
  const IndexRange points = curves.points_by_curve()[curve];

  bke::GSpanAttributeWriter selection = ensure_selection_attribute(
      curves, bke::AttrDomain::Point, CD_PROP_BOOL);

  IndexMaskMemory memory;
  const IndexMask selected = selection_from_modified(point_weights, memory);
  fill_selection_true(selection.span.slice(points), selected);
  selection.finish();
}

void gather_modified_positions(const Span<float3> positions,
                               const Span<float> weights,
                               const WeightMatrix &point_weights,
                               const IndexMask selection,
                               MutableSpan<float3> r_positions)
{
  bke::attribute_math::DefaultMixer<float3> mixer{r_positions};

  selection.foreach_index(GrainSize(128), [&](const int row, const int index) {
    for (WeightMatrix::InnerIterator it(point_weights, row); it; ++it) {
      const int col = it.col();
      mixer.mix_in(index, positions[col], weights[col] * it.value());
    }
  });
  mixer.finalize();
}

static void apply_weights_to_curve(const bke::CurvesGeometry &src_curves,
                                   const WeightMatrix &point_weights,
                                   const int curve,
                                   bke::CurvesGeometry &dst_curves)
{
  const IndexRange curve_points = src_curves.points_by_curve()[curve];
  const IndexRange new_curve_points = IndexRange::from_begin_size(curve_points.first(),
                                                                  point_weights.rows());
  const int new_points_added = new_curve_points.size() - curve_points.size();
  const IndexRange points_before = IndexRange::from_begin_end(0, curve_points.first());
  const IndexRange points_after = IndexRange::from_begin_end(curve_points.one_after_last(),
                                                             src_curves.points_num());

  const bke::AttributeAccessor src_attributes = src_curves.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();

  Array<float> weights_buffer;
  Span<float> weights = prepare_curve_weights(
      src_curves.nurbs_weights(), curve_points, weights_buffer);

  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_POINT,
           bke::attribute_filter_from_skip_ref(
               ed::curves::get_curves_selection_attribute_names(src_curves))))
  {
    GSpan points_before_curve = attribute.src.slice(points_before);
    GSpan points_after_curve = attribute.src.slice(points_after);
    attribute.dst.span.slice(points_before).copy_from(points_before_curve);
    attribute.dst.span.slice(points_after.shift(new_points_added)).copy_from(points_after_curve);

    bke::attribute_math::convert_to_static_type(attribute.dst.span.type(), [&](auto dummy) {
      using T = decltype(dummy);
      if constexpr (!std::is_void_v<bke::attribute_math::DefaultMixer<T>>) {
        Span<T> src_points = attribute.src.typed<T>().slice(curve_points);
        bke::attribute_math::DefaultMixer<T> mixer{
            attribute.dst.span.typed<T>().slice(new_curve_points)};

        threading::parallel_for(IndexRange(point_weights.rows()), 512, [&](IndexRange rows) {
          for (const int row : rows) {
            for (WeightMatrix::InnerIterator it(point_weights, row); it; ++it) {
              const int src_point = it.col();
              mixer.mix_in(row, src_points[src_point], weights[src_point] * it.value());
            }
          }
        });
        mixer.finalize();
      }
    });
    attribute.dst.finish();
  }
}

static void insert_knot_value(const int points_num,
                              const Span<float> src_knots,
                              const float knot,
                              const int knot_span,
                              const int repeat,
                              MutableSpan<float> dst_knots)
{
  const bool loop_to_front = knot_span >= points_num;
  const int first_stable_knot = loop_to_front ? (knot_span % points_num) + 1 : 0;
  const IndexRange stable_knots = IndexRange::from_begin_end_inclusive(first_stable_knot,
                                                                       knot_span);
  dst_knots.slice(stable_knots).copy_from(src_knots.slice(stable_knots));
  dst_knots.slice(IndexRange::from_begin_size(knot_span + 1, repeat)).fill(knot);
  MutableSpan<float> after_insertion = dst_knots.drop_front(stable_knots.one_after_last() +
                                                            repeat);
  after_insertion.copy_from(src_knots.slice(knot_span + 1, after_insertion.size()));
  /* For cyclic curves only, when new knots are inserted in front and somewhere after
   * knots[curve_points.size()]. */
  for (const int k : IndexRange::from_begin_end(0, first_stable_knot)) {
    dst_knots[first_stable_knot - 1 - k] = dst_knots[first_stable_knot] +
                                           dst_knots[knot_span + repeat - k] -
                                           src_knots[knot_span + 1];
  }
}

bke::CurvesGeometry insert_knot(const bke::CurvesGeometry &curves,
                                const int curve,
                                const float knot,
                                const int knot_span,
                                const int knot_multiplicity,
                                const int repeat,
                                const Span<float> knots)
{
  const IndexRange curve_points = curves.points_by_curve()[curve];
  const int8_t order = curves.nurbs_orders()[curve];
  BLI_assert(repeat > 0);
  BLI_assert(knot_multiplicity + repeat < order);
  BLI_assert(order - 1 <= knot_span && knot_span < curve_points.size() + order - 1);

  /* Create new curves object and update point offsets. */
  const int new_points_added = repeat;
  bke::CurvesGeometry new_curves = bke::curves::copy_only_curve_domain(curves);
  BKE_defgroup_copy_list(&new_curves.vertex_group_names, &curves.vertex_group_names);
  new_curves.resize(curves.points_num() + new_points_added, curves.curves_num());
  new_curves.nurbs_knots_modes_for_write()[curve] = NURBS_KNOT_MODE_CUSTOM;
  MutableSpan<int> new_offsets = new_curves.offsets_for_write();
  new_offsets.copy_from(curves.offsets());
  for (const int i : new_curves.curves_range().drop_front(curve)) {
    new_offsets[i + 1] += new_points_added;
  }
  new_curves.nurbs_custom_knots_update_size();

  /* Copy knots of all other curves. */
  const Span<float> src_knots_all = curves.nurbs_custom_knots();
  const OffsetIndices<int> new_knots_by_curve = new_curves.nurbs_custom_knots_by_curve();
  const IndexRange curve_knots = new_knots_by_curve[curve];
  const IndexRange before_curve = IndexRange::from_begin_end(0, curve_knots.start());
  const IndexRange after_curve = IndexRange::from_begin_end(curve_knots.one_after_last(),
                                                            new_knots_by_curve.total_size());
  MutableSpan<float> new_knots_all = new_curves.nurbs_custom_knots_for_write();
  array_utils::copy<float>(src_knots_all.take_front(before_curve.size()),
                           new_knots_all.slice(before_curve));
  array_utils::copy<float>(src_knots_all.take_back(after_curve.size()),
                           new_knots_all.slice(after_curve));

  /* Fill new knots of curve being modified. */
  insert_knot_value(
      curve_points.size(), knots, knot, knot_span, repeat, new_knots_all.slice(curve_knots));

  const WeightMatrix weights = calc_knot_insertion_weights(
      knots, curve_points.size(), order, knot, knot_span, knot_multiplicity, repeat);

  apply_weights_to_curve(curves, weights, curve, new_curves);
  select_curve_points_modified_by_weights(weights, curve, new_curves);

  return new_curves;
}
}  // namespace blender::ed::curves::nurbs
