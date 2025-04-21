#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "ED_curves.hh"

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

IndexRange calc_knot_insertion_weights(const Span<float> knots,
                                       const int8_t order,
                                       const float knot,
                                       const int knot_span,
                                       const int mult,
                                       const int repeat,
                                       MutableSpan<float> insertion_weights)
{
  BLI_assert(mult + repeat < order);
  const int altered_point_num = order - mult + repeat - 2;
  const IndexRange points_to_replace = IndexRange::from_begin_size(knot_span - order + 2,
                                                                   altered_point_num - repeat);

  Array<float> point_weights_buffer(order * order, 0.0f);
  MutableSpan<float> point_weights = point_weights_buffer.as_mutable_span();
  for (const int i : IndexRange(order)) {
    point_weights[i * order + i] = 1.0f;
  }

  for (const int r : IndexRange::from_begin_size(1, repeat)) {
    const int leg = knot_span - order + 1 + r;
    for (const int i : IndexRange(order - r - mult)) {
      const float alpha = (knot - knots[leg + i]) / (knots[i + knot_span + 1] - knots[leg + i]);
      const MutableSpan<float> q_i_weights = point_weights.slice(i * order, order);
      const Span<float> q_i_1_weights = point_weights.slice((i + 1) * order, order);
      for (const int point : IndexRange(order)) {
        q_i_weights[point] = alpha * q_i_1_weights[point] + (1.0f - alpha) * q_i_weights[point];
      }
    }
    insertion_weights.slice((r - 1) * order, order).copy_from(point_weights.slice(0, order));
    insertion_weights.slice((altered_point_num - r) * order, order)
        .copy_from(point_weights.slice((order - 1 - r - mult) * order, order));
  }

  for (const int i : IndexRange::from_begin_size(1, std::max(order - mult - repeat - 2, 0))) {
    insertion_weights.slice(i * order, order).copy_from(point_weights.slice(i * order, order));
  }

  return points_to_replace;
}

Array<float> make_weights_for_knot_span(const int order,
                                        const Span<float> all_weights,
                                        const IndexRange curve_points,
                                        const int knot_span)
{
  Array<float> weights(order, 1.0f);
  if (!all_weights.is_empty()) {
    const Span<float> curve_weights = all_weights.slice(curve_points);
    for (const int i : IndexRange(order)) {
      weights[i] = curve_weights[(knot_span - order + 1 + i) % curve_points.size()];
    }
  }
  return weights;
}

bke::CurvesGeometry insert_knot(const bke::CurvesGeometry &curves,
                                const int curve,
                                const float knot,
                                const int repeat)
{
  const IndexRange curve_points = curves.points_by_curve()[curve];
  const bool cyclic = curves.cyclic()[curve];
  const int8_t order = curves.nurbs_orders()[curve];
  const int knots_num = bke::curves::nurbs::knots_num(curve_points.size(), order, cyclic);

  Array<float> knots_buffer;
  knots_buffer.reinitialize(knots_num);
  bke::curves::nurbs::load_curve_knots(KnotsMode(curves.nurbs_knots_modes()[curve]),
                                       curve_points.size(),
                                       order,
                                       cyclic,
                                       curves.nurbs_custom_knots_by_curve()[curve],
                                       curves.nurbs_custom_knots(),
                                       knots_buffer);

  const Span<float> knots = knots_buffer.as_span();

  int knot_span;
  int knot_multiplicity;
  find_span_mult(knot, knots, order, knot_span, knot_multiplicity);

  BLI_assert(order - 1 <= knot_span && knot_span < curve_points.size() + order - 1);

  /* Create new curves object and update point offsets. */
  const int new_points_added = repeat;
  bke::CurvesGeometry new_curves = bke::curves::copy_only_curve_domain(curves);
  bke::curves::copy_custom_knots(curves, new_curves);
  new_curves.resize(curves.points_num() + new_points_added, curves.curves_num());
  new_curves.nurbs_knots_modes_for_write()[curve] = NURBS_KNOT_MODE_CUSTOM;
  MutableSpan<int> new_offsets = new_curves.offsets_for_write();
  new_offsets.copy_from(curves.offsets());
  for (const int i : new_curves.curves_range().drop_front(curve)) {
    new_offsets[i + 1] += new_points_added;
  }
  new_curves.nurbs_custom_knots_update_size();

  /* Shift knots of curves stored after curve being modified. */
  const OffsetIndices<int> new_knots_by_curve = new_curves.nurbs_custom_knots_by_curve();
  const IndexRange curve_knots = new_knots_by_curve[curve];
  const IndexRange tail = IndexRange::from_begin_end(curve_knots.one_after_last(),
                                                     new_knots_by_curve.total_size());
  MutableSpan<float> new_knots_all = new_curves.nurbs_custom_knots_for_write();
  new_knots_all.slice(tail).copy_from(curves.nurbs_custom_knots().take_back(tail.size()));

  /* Fill new knots of curve being modified. */
  const IndexRange new_curve_points = IndexRange::from_begin_size(
      curve_points.first(), curve_points.size() + new_points_added);
  const bool loop_to_front = knot_span >= curve_points.size();
  const int first_stable_knot = loop_to_front ? (knot_span % curve_points.size()) + 1 : 0;
  const IndexRange stable_knots = IndexRange::from_begin_end_inclusive(first_stable_knot,
                                                                       knot_span);
  MutableSpan<float> new_knots = new_knots_all.slice(curve_knots);
  new_knots.slice(stable_knots).copy_from(knots.slice(stable_knots));
  new_knots.slice(IndexRange::from_begin_size(knot_span + 1, new_points_added)).fill(knot);
  MutableSpan<float> after_insertion = new_knots.drop_front(stable_knots.one_after_last() +
                                                            new_points_added);
  after_insertion.copy_from(knots.slice(knot_span + 1, after_insertion.size()));
  /* For cyclic curves only, when new knots are inserted in front and somewhere after
   * knots[curve_points.size()]. */
  for (const int k : IndexRange::from_begin_end(0, first_stable_knot)) {
    new_knots[first_stable_knot - 1 - k] = new_knots[first_stable_knot] +
                                           new_knots[knot_span + new_points_added - k] -
                                           knots[knot_span + 1];
  }

  const int max_altered_point_count = 2 * (order - 1) - 1;
  Array<float> point_weights_buffer(max_altered_point_count * order);
  const IndexRange points_to_replace = calc_knot_insertion_weights(
      knots, order, knot, knot_span, knot_multiplicity, repeat, point_weights_buffer);

  /* Update point attributes. */
  const bke::AttributeAccessor src_attributes = curves.attributes();
  bke::MutableAttributeAccessor dst_attributes = new_curves.attributes_for_write();

  const IndexRange points_before = IndexRange::from_begin_end(
      0,
      std::min(curve_points.one_after_last(), curve_points.first() + points_to_replace.start()));
  const IndexRange points_after = IndexRange::from_begin_end(
      std::min(curve_points.one_after_last(), curve_points.first() + points_to_replace.last()),
      curves.points_num());

  Array<bool> is_altered(new_curve_points.size());
  const IndexRange altered_points_range = IndexRange::from_begin_size(
      points_to_replace.first(), points_to_replace.size() + new_points_added);
  for (const int i : altered_points_range) {
    is_altered[i % new_curve_points.size()] = true;
  }
  IndexMaskMemory memory;
  const IndexMask altered_points = IndexMask::from_bools(
      IndexRange(new_curve_points.size()), is_altered, memory);
  const Array<float> weights = make_weights_for_knot_span(
      order, curves.nurbs_weights(), curve_points, knot_span);

  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_POINT,
           bke::attribute_filter_from_skip_ref(
               ed::curves::get_curves_selection_attribute_names(curves))))
  {
    GSpan points_before_span = attribute.src.slice(points_before);
    GSpan points_after_span = attribute.src.slice(points_after);
    attribute.dst.span.slice(points_before).copy_from(points_before_span);
    attribute.dst.span.slice(points_after.shift(new_points_added)).copy_from(points_after_span);

    bke::attribute_math::convert_to_static_type(attribute.dst.span.type(), [&](auto dummy) {
      using T = decltype(dummy);
      if constexpr (!std::is_void_v<bke::attribute_math::DefaultMixer<T>>) {
        Span<T> src_points = attribute.src.typed<T>().slice(curve_points);
        bke::attribute_math::DefaultMixer<T> mixer{
            attribute.dst.span.typed<T>().slice(new_curve_points), altered_points};

        for (const int j : altered_points_range.index_range()) {
          const int altered_point = (altered_points_range[j]) % new_curve_points.size();
          Span<float> point_weights = point_weights_buffer.as_span().slice(j * order, order);
          for (const int i : point_weights.index_range()) {
            const int src_i = (knot_span - order + 1 + i) % curve_points.size();
            mixer.mix_in(altered_point, src_points[src_i], weights[i] * point_weights[i]);
          }
        };
        mixer.finalize(altered_points);
      }
    });
    attribute.dst.finish();
  }
  OffsetIndices<int> new_points_by_curve = new_curves.points_by_curve();
  foreach_selection_attribute_writer(
      new_curves, bke::AttrDomain::Point, [&](bke::GSpanAttributeWriter &selection) {
        for (const int curve : new_curves.curves_range()) {
          fill_selection_false(selection.span.slice(new_points_by_curve[curve]));
        }
        fill_selection_true(selection.span.slice(new_points_by_curve[curve]), altered_points);
      });

  return new_curves;
}
}  // namespace blender::ed::curves::nurbs
