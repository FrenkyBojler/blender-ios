/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector_types.hh"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_layer.hh"

#include "DEG_depsgraph.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "ED_curves.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::curves {

/* Project an object-space curve point into the active region for screen-space distance checks. */
static bool curves_rip_edge_point_to_screen(const ARegion *region,
                                            const float3 &co,
                                            float r_co[2])
{
  return ED_view3d_project_float_object(
             region, co, r_co, V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_WIN) ==
         V3D_PROJ_RET_OK;
}

/* Return the adjacent point index in curve-local coordinates, wrapping cyclic curves when needed. */
static int curves_rip_edge_neighbor_index_get(const IndexRange points,
                                              const bool cyclic,
                                              const int index,
                                              const int direction)
{
  if (direction < 0) {
    if (index > 0) {
      return index - 1;
    }
    if (cyclic) {
      return points.size() - 1;
    }
  }
  else {
    if (index + 1 < points.size()) {
      return index + 1;
    }
    if (cyclic) {
      return 0;
    }
  }

  return -1;
}

/* Measure the screen-space cursor distance to a poly segment. */
static float curves_rip_edge_poly_segment_dist_sq(const ARegion *region,
                                                  const bke::CurvesGeometry &curves,
                                                  const int curve,
                                                  const int segment,
                                                  const float mval_fl[2])
{
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const IndexRange points = points_by_curve[curve];
  const bool cyclic = curves.cyclic()[curve];
  /* Non-cyclic curves do not have a segment starting at the final point. */
  const int segment_next = (segment + 1 < points.size()) ? segment + 1 : (cyclic ? 0 : -1);
  if (segment_next < 0) {
    return FLT_MAX;
  }

  const Span<float3> positions = curves.positions();
  float point1[2], point2[2];
  /* Ignore segments with endpoints clipped out of the viewport. */
  if (!curves_rip_edge_point_to_screen(region, positions[points[segment]], point1)) {
    return FLT_MAX;
  }
  if (!curves_rip_edge_point_to_screen(region, positions[points[segment_next]], point2)) {
    return FLT_MAX;
  }
  return dist_squared_to_line_segment_v2(mval_fl, point1, point2);
}

/* Measure the cursor distance to a contiguous evaluated segment, wrapping cyclic endings. */
static float curves_rip_edge_evaluated_segment_dist_sq(const ARegion *region,
                                                       const bke::CurvesGeometry &curves,
                                                       const int curve,
                                                       const IndexRange evaluated_segment,
                                                       const float mval_fl[2])
{
  const OffsetIndices<int> evaluated_points_by_curve = curves.evaluated_points_by_curve();
  const Span<float3> evaluated_positions = curves.evaluated_positions();
  const IndexRange evaluated_points = evaluated_points_by_curve[curve];
  if (evaluated_points.is_empty() || evaluated_segment.size() <= 1) {
    return FLT_MAX;
  }

  const int evaluated_segments_num = evaluated_segment.size() - 1;

  float min_dist_sq = FLT_MAX;
  float point1[2], point2[2];
  const int first_point = (evaluated_segment.first() - evaluated_points.first()) %
                              evaluated_points.size() +
                          evaluated_points.first();
  bool has_point1 = curves_rip_edge_point_to_screen(
      region, evaluated_positions[first_point], point1);
  if (has_point1) {
    /* Include the first visible endpoint so heavily clipped segments still get a useful distance. */
    min_dist_sq = min_ff(min_dist_sq, len_squared_v2v2(mval_fl, point1));
  }

  for (const int evaluated_i : IndexRange(evaluated_segments_num)) {
    const int point_i = evaluated_segment.first() + evaluated_i + 1;
    const int point_i_wrapped = (point_i - evaluated_points.first()) % evaluated_points.size() +
                                evaluated_points.first();
    if (!curves_rip_edge_point_to_screen(region, evaluated_positions[point_i_wrapped], point2)) {
      continue;
    }
    if (has_point1) {
      min_dist_sq = min_ff(min_dist_sq, dist_squared_to_line_segment_v2(mval_fl, point1, point2));
    }
    copy_v2_v2(point1, point2);
    has_point1 = true;
  }

  return min_dist_sq;
}

/* Approximate screen-space cursor distance to a Bezier segment using its evaluated polyline. */
static float curves_rip_edge_bezier_segment_dist_sq(const ARegion *region,
                                                    const bke::CurvesGeometry &curves,
                                                    const int curve,
                                                    const int segment,
                                                    const float mval_fl[2])
{
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const IndexRange points = points_by_curve[curve];
  const bool cyclic = curves.cyclic()[curve];
  /* Non-cyclic curves do not have a segment starting at the final control point. */
  const int segment_next = (segment + 1 < points.size()) ? segment + 1 : (cyclic ? 0 : -1);
  if (segment_next < 0) {
    return FLT_MAX;
  }

  const OffsetIndices<int> evaluated_points_by_curve = curves.evaluated_points_by_curve();
  const IndexRange evaluated_points = evaluated_points_by_curve[curve];
  const Span<int> offsets = curves.bezier_evaluated_offsets_for_curve(curve);

  /* Bezier handles bend the visible segment, so test against the evaluated polyline span. */
  const IndexRange evaluated_segment = IndexRange::from_begin_end_inclusive(offsets[segment],
                                                                           offsets[segment + 1])
                                           .shift(evaluated_points.first());
  return curves_rip_edge_evaluated_segment_dist_sq(
      region, curves, curve, evaluated_segment, mval_fl);
}

/* Approximate screen-space cursor distance to a Catmull Rom segment using evaluated samples. */
static float curves_rip_edge_catmull_rom_segment_dist_sq(const ARegion *region,
                                                         const bke::CurvesGeometry &curves,
                                                         const int curve,
                                                         const int segment,
                                                         const float mval_fl[2])
{
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const IndexRange points = points_by_curve[curve];
  const bool cyclic = curves.cyclic()[curve];
  /* Non-cyclic curves do not have a segment starting at the final control point. */
  const int segment_next = (segment + 1 < points.size()) ? segment + 1 : (cyclic ? 0 : -1);
  if (segment_next < 0) {
    return FLT_MAX;
  }

  const OffsetIndices<int> evaluated_points_by_curve = curves.evaluated_points_by_curve();
  const IndexRange evaluated_points = evaluated_points_by_curve[curve];
  const int resolution = curves.resolution()[curve];
  const int evaluated_points_per_segment = (resolution > 1) ? resolution : 1;
  const IndexRange evaluated_segment = IndexRange::from_begin_size(
      evaluated_points.first() + segment * evaluated_points_per_segment,
      evaluated_points_per_segment + 1);
  return curves_rip_edge_evaluated_segment_dist_sq(
      region, curves, curve, evaluated_segment, mval_fl);
}

/* Calculate the screen-space segment distance for editable curve types supported by this operator. */
static float curves_rip_edge_segment_dist_sq(const ARegion *region,
                                             const bke::CurvesGeometry &curves,
                                             const int curve,
                                             const int segment,
                                             const float mval_fl[2])
{
  if (segment < 0) {
    return FLT_MAX;
  }

  const int8_t curve_type = curves.curve_types()[curve];
  if (curve_type == CURVE_TYPE_BEZIER) {
    return curves_rip_edge_bezier_segment_dist_sq(region, curves, curve, segment, mval_fl);
  }
  if (curve_type == CURVE_TYPE_CATMULL_ROM) {
    return curves_rip_edge_catmull_rom_segment_dist_sq(region, curves, curve, segment, mval_fl);
  }
  if (ELEM(curve_type, CURVE_TYPE_POLY, CURVE_TYPE_NURBS)) {
    /* NURBS side picking uses the control polygon because CurvesGeometry does not expose
     * per-control-point evaluated spans for its curve. */
    return curves_rip_edge_poly_segment_dist_sq(region, curves, curve, segment, mval_fl);
  }
  return FLT_MAX;
}

/* Choose which adjacent side of a selected point should receive the duplicate point. */
static int curves_rip_edge_side_get(const ARegion *region,
                                    const bke::CurvesGeometry &curves,
                                    const int curve,
                                    const int point,
                                    const float mval_fl[2])
{
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const IndexRange points = points_by_curve[curve];
  if (points.size() <= 1) {
    return 0;
  }

  const int point_in_curve = point - points.first();
  const bool cyclic = curves.cyclic()[curve];
  const int prev_index = curves_rip_edge_neighbor_index_get(points, cyclic, point_in_curve, -1);
  const int next_index = curves_rip_edge_neighbor_index_get(points, cyclic, point_in_curve, 1);

  /* Non-cyclic endpoints have only one adjacent segment, so duplicate on that side. */
  if (prev_index == -1 && next_index == -1) {
    return 0;
  }
  if (prev_index == -1) {
    return 1;
  }
  if (next_index == -1) {
    return -1;
  }
  /* In two-point cyclic Poly curves, both adjacent directions refer to the same straight segment. */
  if (prev_index == next_index && curves.curve_types()[curve] == CURVE_TYPE_POLY) {
    return 1;
  }

  /* For points with two distinct adjacent segments, choose the segment nearest to the cursor. */
  const float dist_prev = curves_rip_edge_segment_dist_sq(
      region, curves, curve, prev_index, mval_fl);
  const float dist_next = curves_rip_edge_segment_dist_sq(
      region, curves, curve, point_in_curve, mval_fl);
  return (dist_prev < dist_next) ? -1 : 1;
}

/* Return whether a point should be ripped, including Bézier handle-only selection. */
static bool curves_rip_edge_is_point_selected(const VArray<bool> &selection,
                                              const VArray<bool> &selection_left,
                                              const VArray<bool> &selection_right,
                                              const int8_t curve_type,
                                              const int point)
{
  if (selection[point]) {
    return true;
  }
  if (curve_type != CURVE_TYPE_BEZIER) {
    return false;
  }
  return selection_left[point] || selection_right[point];
}

/* Duplicate selected points beside the chosen adjacent segment, preserving point attributes. */
static bool curves_rip_edge(bke::CurvesGeometry &curves,
                            const ARegion &region,
                            const float mval_fl[2])
{
  if (curves.points_num() == 0) {
    return false;
  }

  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const VArray<int8_t> curve_types = curves.curve_types();
  const bke::AttributeAccessor src_attributes = curves.attributes();
  const VArray<bool> selection = *src_attributes.lookup_or_default<bool>(
      ".selection", bke::AttrDomain::Point, true);
  const bool has_bezier = curves.has_curve_with_type(CURVE_TYPE_BEZIER);
  const VArray<bool> selection_left = has_bezier ?
                                          *src_attributes.lookup_or_default<bool>(
                                              ".selection_handle_left",
                                              bke::AttrDomain::Point,
                                              true) :
                                          VArray<bool>::from_single(false, curves.points_num());
  const VArray<bool> selection_right = has_bezier ?
                                           *src_attributes.lookup_or_default<bool>(
                                               ".selection_handle_right",
                                               bke::AttrDomain::Point,
                                               true) :
                                           VArray<bool>::from_single(false, curves.points_num());

  /* First pass: determine which selected points can be duplicated and where to insert them. */
  Array<int8_t> insert_side(curves.points_num(), 0);
  Array<bool> changed_curves(curves.curves_num(), false);
  int new_points_num = 0;
  for (const int curve : curves.curves_range()) {
    /* Only curve types with rip-edge side selection implemented below. */
    if (!ELEM(curve_types[curve],
              CURVE_TYPE_CATMULL_ROM,
              CURVE_TYPE_POLY,
              CURVE_TYPE_BEZIER,
              CURVE_TYPE_NURBS))
    {
      continue;
    }

    const IndexRange points = points_by_curve[curve];
    for (const int point : points) {
      if (!curves_rip_edge_is_point_selected(
              selection, selection_left, selection_right, curve_types[curve], point))
      {
        continue;
      }

      const int side = curves_rip_edge_side_get(&region, curves, curve, point, mval_fl);
      if (side != 0) {
        insert_side[point] = side;
        changed_curves[curve] = true;
        new_points_num++;
      }
    }
  }

  /* Nothing selected produced an extendable segment, so leave the geometry unchanged. */
  if (new_points_num == 0) {
    return false;
  }

  Array<int> new_offsets(curves.curves_num() + 1);
  Array<int> dst_to_src_point(curves.points_num() + new_points_num);
  Array<bool> dst_is_duplicate(curves.points_num() + new_points_num, false);

  int dst_point = 0;
  new_offsets.first() = 0;
  /* Build the new point order by inserting each duplicate immediately before or after its source. */
  for (const int curve : curves.curves_range()) {
    for (const int point : points_by_curve[curve]) {
      if (insert_side[point] == -1) {
        dst_to_src_point[dst_point] = point;
        dst_is_duplicate[dst_point] = true;
        dst_point++;
      }

      dst_to_src_point[dst_point] = point;
      dst_is_duplicate[dst_point] = false;
      dst_point++;

      if (insert_side[point] == 1) {
        dst_to_src_point[dst_point] = point;
        dst_is_duplicate[dst_point] = true;
        dst_point++;
      }
    }
    new_offsets[curve + 1] = dst_point;
  }

  bke::CurvesGeometry new_curves = bke::curves::copy_only_curve_domain(curves);
  new_curves.resize(dst_point, curves.curves_num());
  new_curves.offsets_for_write().copy_from(new_offsets);
  /* Changed NURBS curves have a different point count, so their custom knots can no longer be
   * copied directly. Preserve custom knots only for unchanged NURBS curves and normalize changed
   * curves to generated knot modes. */
  if (curves.nurbs_has_custom_knots()) {
    IndexMaskMemory memory;
    const IndexMask changed_nurbs_curves = IndexMask::from_predicate(
        curves.curves_range(), memory, [&](const int64_t curve) {
          return changed_curves[curve] && curve_types[curve] == CURVE_TYPE_NURBS;
        });
    bke::curves::nurbs::copy_custom_knots(curves, changed_nurbs_curves, new_curves);
  }

  bke::MutableAttributeAccessor dst_attributes = new_curves.attributes_for_write();
  /* Copy point attributes from each source point to both original and duplicate outputs. */
  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           {bke::AttrDomain::Point},
           bke::attribute_filter_from_skip_ref(get_curves_selection_attribute_names(curves))))
  {
    bke::attribute_math::gather(attribute.src, dst_to_src_point, attribute.dst.span);
    attribute.dst.finish();
  }

  /* Write selection separately so only the newly inserted duplicate points remain selected. */
  for (const StringRef selection_name : get_curves_selection_attribute_names(new_curves)) {
    const VArray<bool> src_selection = *src_attributes.lookup_or_default<bool>(
        selection_name, bke::AttrDomain::Point, true);
    bke::SpanAttributeWriter<bool> dst_selection_attribute =
        dst_attributes.lookup_or_add_for_write_span<bool>(selection_name, bke::AttrDomain::Point);
    MutableSpan<bool> dst_selection = dst_selection_attribute.span;
    /* For every selection layer, select inserted duplicates for transform, deselect ripped source
     * points, and preserve selection on untouched points. */
    for (const int dst_i : dst_selection.index_range()) {
      if (dst_is_duplicate[dst_i]) {
        dst_selection[dst_i] = true;
      }
      else if (insert_side[dst_to_src_point[dst_i]] != 0) {
        dst_selection[dst_i] = false;
      }
      else {
        dst_selection[dst_i] = src_selection[dst_to_src_point[dst_i]];
      }
    }
    dst_selection_attribute.finish();
  }

  /* Refresh cached curve type data and Bezier handles after rebuilding the point topology. */
  new_curves.update_curve_types();
  new_curves.tag_topology_changed();
  new_curves.calculate_bezier_auto_handles();
  curves = std::move(new_curves);
  return true;
}

/* Execute the rip-edge operator on all editable curves using point-domain selection. */
static wmOperatorStatus curves_rip_edge_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

  float mval_fl[2];
  RNA_float_get_array(op->ptr, "mouse", mval_fl);

  bool changed = false;
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, v3d);
  /* Process multi-object edit mode while avoiding repeated edits of shared curve data-blocks. */
  for (Object *obedit : objects) {
    if (obedit->type != OB_CURVES) {
      continue;
    }

    Curves *curves_id = id_cast<Curves *>(obedit->data);
    if (bke::AttrDomain(curves_id->selection_domain) != bke::AttrDomain::Point) {
      continue;
    }

    /* Projection uses object matrices, so update the view matrices for each edited object. */
    ED_view3d_init_mats_rv3d(obedit, rv3d);
    bke::CurvesGeometry &curves = curves_id->geometry.wrap();
    if (!curves_rip_edge(curves, *region, mval_fl)) {
      continue;
    }

    DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves_id);
    changed = true;
  }

  return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

/* Store the invoking cursor position so redo uses the same side-selection input. */
static wmOperatorStatus curves_rip_edge_invoke(bContext *C,
                                               wmOperator *op,
                                               const wmEvent *event)
{
  const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
  RNA_float_set_array(op->ptr, "mouse", mval_fl);
  return curves_rip_edge_exec(C, op);
}

/* Register the curve edit-mode operator that extends selected vertices from nearby segments. */
void CURVES_OT_rip_edge(wmOperatorType *ot)
{
  ot->name = "Extend Vertices";
  ot->idname = "CURVES_OT_rip_edge";
  ot->description = "Extend vertices along the curve segment closest to the cursor";

  ot->invoke = curves_rip_edge_invoke;
  ot->exec = curves_rip_edge_exec;
  ot->poll = editable_curves_in_edit_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  PropertyRNA *prop = RNA_def_float_vector(ot->srna,
                                           "mouse",
                                           2,
                                           nullptr,
                                           -FLT_MAX,
                                           FLT_MAX,
                                           "Mouse",
                                           "Screen-space cursor position for choosing the extend "
                                           "side",
                                           -1.0f,
                                           1.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

}  // namespace blender::ed::curves
