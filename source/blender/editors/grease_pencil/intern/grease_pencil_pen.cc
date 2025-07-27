/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 * Operator for creating splines in Grease Pencil.
 */

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"

#include "BLI_array_utils.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "DEG_depsgraph.hh"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "UI_resources.hh"

namespace blender::ed::greasepencil {

static const EnumPropertyItem prop_handle_types[] = {
    {BEZIER_HANDLE_AUTO, "AUTO", 0, "Auto", ""},
    {BEZIER_HANDLE_VECTOR, "VECTOR", 0, "Vector", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class PenModal : int8_t {
  FreeAlignToggle = 0,
  MoveAdjacent = 1,
  MoveEntire = 2,
  LinkHandles = 3,
  LockAngle = 4,
};

enum class ElementMode : int8_t {
  None = 0,
  Point = 1,
  Edge = 2,
  HandleLeft = 3,
  HandleRight = 4,
};

struct ClosestElement {
  ElementMode element_mode;
  int point_index = -1;
  int curve_index = -1;
  float edge_t = -1.0f;
  int layer_index = -1;
};

/* Used to scale the default select distance. */
constexpr float selection_distance_factor = 0.9f;
constexpr float selection_distance_factor_edge = 0.5f;
/* Used when creating a single curve from nothing. */
constexpr float default_handle_px_distance = 16.0f;
constexpr float default_radius_factor = 0.25f;

struct PenToolOperation {
  ViewContext vc;

  GreasePencil *grease_pencil;
  Vector<MutableDrawingInfo> drawings;

  float threshold_distance;
  float threshold_distance_edge;

  bool extrude_point;
  bool delete_point;
  bool insert_point;
  bool move_seg;
  bool select_point;
  bool move_point;
  bool close_spline;
  bool cycle_handle_type;
  int extrude_handle;

  bool point_added;
  bool point_removed;

  float4x4 projection;
  float2 mouse_co;
  float2 center_of_mass_co;

  ClosestElement closest_element;
};

static void grease_pencil_pen_update_view(bContext *C, PenToolOperation &ptd)
{
  GreasePencil *grease_pencil = ptd.grease_pencil;

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, grease_pencil);

  ED_region_tag_redraw(ptd.vc.region);
}

static float2 pen_layer_to_screen(const PenToolOperation &ptd,
                                  const float4x4 &layer_to_object,
                                  const float3 &point)
{
  return ED_view3d_project_float_v2_m4(
      ptd.vc.region, math::transform_point(layer_to_object, point), ptd.projection);
}

static float3 pen_screen_to_layer(const PenToolOperation &ptd,
                                  const float4x4 &layer_to_world,
                                  const float2 screen_co,
                                  const float3 depth_point_layer)
{
  const float3 depth_point = math::transform_point(layer_to_world, depth_point_layer);
  float3 proj_point;
  ED_view3d_win_to_3d(ptd.vc.v3d, ptd.vc.region, depth_point, screen_co, proj_point);
  return math::transform_point(math::invert(layer_to_world), proj_point);
}

/* Will return -1 if no points are near. */
static int pen_find_closest_point_or_handle(const PenToolOperation &ptd,
                                            const bke::greasepencil::Drawing &drawing,
                                            const int layer_index,
                                            const float2 mouse_co,
                                            int *r_closest_curve,
                                            ElementMode *r_element_mode)
{
  float closest_distance_squared = std::numeric_limits<float>::max();
  int closest_point = -1;

  const bke::CurvesGeometry &curves = drawing.strokes();
  const Span<float3> positions = curves.positions();

  const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(layer_index);
  const float4x4 layer_to_object = layer.local_transform();

  IndexMaskMemory memory;
  const IndexMask editable_points = ed::greasepencil::retrieve_editable_points(
      *ptd.vc.obact, drawing, layer_index, memory);
  editable_points.foreach_index([&](const int point_i) {
    const float2 pos_proj = pen_layer_to_screen(ptd, layer_to_object, positions[point_i]);
    const float distance_squared = math::distance_squared(pos_proj, mouse_co);

    /* Save the closest point. */
    if (distance_squared < closest_distance_squared &&
        distance_squared < ptd.threshold_distance * ptd.threshold_distance)
    {
      closest_point = point_i;
      const Array<int> point_to_curve_map = curves.point_to_curve_map();
      *r_closest_curve = point_to_curve_map[point_i];
      *r_element_mode = ElementMode::Point;
      closest_distance_squared = distance_squared;
    }
  });

  const Span<float3> handle_left = curves.handle_positions_left();
  const Span<float3> handle_right = curves.handle_positions_right();
  const IndexMask bezier_points = ed::greasepencil::retrieve_visible_bezier_handle_points(
      *ptd.vc.obact, drawing, layer_index, memory);

  bezier_points.foreach_index([&](const int point_i) {
    const float2 pos_proj = pen_layer_to_screen(ptd, layer_to_object, handle_left[point_i]);
    const float distance_squared = math::distance_squared(pos_proj, mouse_co);

    /* Save the closest point. */
    if (distance_squared < closest_distance_squared &&
        distance_squared < ptd.threshold_distance * ptd.threshold_distance)
    {
      closest_point = point_i;
      const Array<int> point_to_curve_map = curves.point_to_curve_map();
      *r_closest_curve = point_to_curve_map[point_i];
      *r_element_mode = ElementMode::HandleLeft;
      closest_distance_squared = distance_squared;
    }
  });

  bezier_points.foreach_index([&](const int point_i) {
    const float2 pos_proj = pen_layer_to_screen(ptd, layer_to_object, handle_right[point_i]);
    const float distance_squared = math::distance_squared(pos_proj, mouse_co);

    /* Save the closest point. */
    if (distance_squared < closest_distance_squared &&
        distance_squared < ptd.threshold_distance * ptd.threshold_distance)
    {
      closest_point = point_i;
      const Array<int> point_to_curve_map = curves.point_to_curve_map();
      *r_closest_curve = point_to_curve_map[point_i];
      *r_element_mode = ElementMode::HandleRight;
      closest_distance_squared = distance_squared;
    }
  });

  return closest_point;
}

static float2 line_segment_closest_point(const float2 pos_1,
                                         const float2 pos_2,
                                         const float2 pos,
                                         float *r_local_t)
{
  const float2 dif_m = pos - pos_1;
  const float2 dif_l = pos_2 - pos_1;
  const float d = math::dot(dif_m, dif_l);
  const float l2 = math::dot(dif_l, dif_l);
  *r_local_t = math::clamp(d / l2, 0.0f, 1.0f);
  return dif_l * (*r_local_t) + pos_1;
}

/* Will return -1 if no points are near. */
static int pen_find_closest_edge_point(const PenToolOperation &ptd,
                                       const bke::greasepencil::Drawing &drawing,
                                       const int layer_index,
                                       const float2 mouse_co,
                                       int *r_closest_curve,
                                       float *r_closest_t)
{
  float closest_distance_squared = std::numeric_limits<float>::max();
  int closest_point = -1;

  const bke::CurvesGeometry &curves = drawing.strokes();
  const OffsetIndices points_by_curve = curves.points_by_curve();
  const OffsetIndices evaluated_points_by_curve = curves.evaluated_points_by_curve();
  const Span<float3> positions = curves.positions();
  const Span<float3> evaluated_positions = curves.evaluated_positions();
  const VArray<bool> curve_cyclic = curves.cyclic();
  const VArray<int8_t> types = curves.curve_types();

  IndexMaskMemory memory;
  const IndexMask editable_curves = ed::greasepencil::retrieve_editable_strokes(
      *ptd.vc.obact, drawing, layer_index, memory);
  const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(layer_index);
  const float4x4 layer_to_object = layer.local_transform();

  editable_curves.foreach_index([&](const int curve_i) {
    const IndexRange src_points = points_by_curve[curve_i];
    const IndexRange eval_points = evaluated_points_by_curve[curve_i];

    const Span<int> offsets = curves.bezier_evaluated_offsets_for_curve(curve_i);
    const bool cyclic = curve_cyclic[curve_i];
    for (const int src_i : src_points.index_range().drop_back(cyclic ? 0 : 1)) {
      if (types[curve_i] != CURVE_TYPE_BEZIER) {
        const int src_i_1 = src_i + src_points.first();
        const int src_i_2 = (src_i + 1) % src_points.size() + src_points.first();
        const float2 pos_1_proj = pen_layer_to_screen(ptd, layer_to_object, positions[src_i_1]);
        const float2 pos_2_proj = pen_layer_to_screen(ptd, layer_to_object, positions[src_i_2]);
        float local_t;
        const float2 closest_pos = line_segment_closest_point(
            pos_1_proj, pos_2_proj, mouse_co, &local_t);

        const float distance_squared = math::distance_squared(closest_pos, mouse_co);
        const float t = local_t;

        /* Save the closest point. */
        if (distance_squared < closest_distance_squared &&
            distance_squared < ptd.threshold_distance_edge * ptd.threshold_distance_edge)
        {
          closest_point = src_points.first() + src_i;
          *r_closest_t = t;
          *r_closest_curve = curve_i;
          closest_distance_squared = distance_squared;
        }
      }
      else {
        const IndexRange eval_range = IndexRange::from_begin_end_inclusive(offsets[src_i],
                                                                           offsets[src_i + 1])
                                          .shift(eval_points.first());
        const int point_num = eval_range.size() - 1;

        for (const int eval_i : IndexRange(point_num)) {
          const int eval_point_i_1 = eval_range.first() + eval_i;
          const int eval_point_i_2 = (eval_range.first() + eval_i + 1 - eval_points.first()) %
                                         eval_points.size() +
                                     eval_points.first();
          const float2 pos_1_proj = pen_layer_to_screen(
              ptd, layer_to_object, evaluated_positions[eval_point_i_1]);
          const float2 pos_2_proj = pen_layer_to_screen(
              ptd, layer_to_object, evaluated_positions[eval_point_i_2]);
          float local_t;
          const float2 closest_pos = line_segment_closest_point(
              pos_1_proj, pos_2_proj, mouse_co, &local_t);

          const float distance_squared = math::distance_squared(closest_pos, mouse_co);
          const float t = (eval_i + local_t) / float(point_num);

          /* Save the closest point. */
          if (distance_squared < closest_distance_squared &&
              distance_squared < ptd.threshold_distance_edge * ptd.threshold_distance_edge)
          {
            closest_point = src_points.first() + src_i;
            *r_closest_t = t;
            *r_closest_curve = curve_i;
            closest_distance_squared = distance_squared;
          }
        }
      }
    }
  });

  if (closest_point == -1) {
    *r_closest_t = -1.0f;
    *r_closest_curve = -1;
  }

  return closest_point;
}

static ClosestElement pen_find_closest_element(const PenToolOperation &ptd,
                                               const bke::greasepencil::Drawing &drawing,
                                               const int layer_index,
                                               const float2 mouse_co)
{
  ClosestElement closest_element;
  int closest_curve;
  ElementMode element_mode;
  const int closest_point = pen_find_closest_point_or_handle(
      ptd, drawing, layer_index, mouse_co, &closest_curve, &element_mode);

  if (closest_point != -1) {
    closest_element.element_mode = element_mode;
    closest_element.curve_index = closest_curve;
    closest_element.point_index = closest_point;
    closest_element.layer_index = layer_index;
    return closest_element;
  }

  float edge_t;
  const int closest_edge_point = pen_find_closest_edge_point(
      ptd, drawing, layer_index, ptd.mouse_co, &closest_curve, &edge_t);

  if (closest_edge_point != -1) {
    closest_element.element_mode = ElementMode::Edge;
    closest_element.point_index = closest_edge_point;
    closest_element.curve_index = closest_curve;
    closest_element.edge_t = edge_t;
    closest_element.layer_index = layer_index;
    return closest_element;
  }

  closest_element.element_mode = ElementMode::None;
  return closest_element;
}

static bke::CurvesGeometry pen_extrude_curves(const PenToolOperation &ptd,
                                              const bke::CurvesGeometry &src,
                                              const float4x4 &layer_to_object,
                                              const float4x4 &layer_to_world,
                                              bool *r_extruded)
{
  const bke::AttributeAccessor src_attributes = src.attributes();
  const OffsetIndices<int> points_by_curve = src.points_by_curve();

  const int old_points_num = src.points_num();

  const VArray<bool> point_selection = *src_attributes.lookup_or_default<bool>(
      ".selection", bke::AttrDomain::Point, true);
  const VArray<bool> left_selected = *src_attributes.lookup_or_default<bool>(
      ".selection_handle_left", bke::AttrDomain::Point, true);
  const VArray<bool> right_selected = *src_attributes.lookup_or_default<bool>(
      ".selection_handle_right", bke::AttrDomain::Point, true);

  Vector<int> dst_to_src_points(old_points_num);
  array_utils::fill_index_range(dst_to_src_points.as_mutable_span());

  Vector<bool> dst_selected_start(old_points_num, false);
  Vector<bool> dst_selected_end(old_points_num, false);

  Vector<int> dst_curve_counts(src.curves_num());
  offset_indices::copy_group_sizes(
      points_by_curve, src.curves_range(), dst_curve_counts.as_mutable_span());

  const VArray<bool> &src_cyclic = src.cyclic();

  /* Point offset keeps track of the points inserted. */
  int point_offset = 0;
  for (const int curve_index : src.curves_range()) {
    const IndexRange curve_points = points_by_curve[curve_index];
    /* Skip cyclic curves unless they only have one point. */
    if (src_cyclic[curve_index] && curve_points.size() != 1) {
      continue;
    }

    if (point_selection[curve_points.first()] || left_selected[curve_points.first()] ||
        right_selected[curve_points.first()])
    {
      if (curve_points.size() != 1) {
        /* Start-point extruded, we insert a new point at the beginning of the curve. */
        dst_to_src_points.insert(curve_points.first() + point_offset, curve_points.first());
        dst_selected_start.insert(curve_points.first() + point_offset, true);
        dst_selected_end.insert(curve_points.first() + point_offset, false);
        dst_curve_counts[curve_index]++;
        point_offset++;
      }
    }

    if (point_selection[curve_points.last()] || left_selected[curve_points.last()] ||
        right_selected[curve_points.last()])
    {
      /* End-point extruded, we insert a new point at the end of the curve. */
      dst_to_src_points.insert(curve_points.last() + point_offset + 1, curve_points.last());
      dst_selected_end.insert(curve_points.last() + point_offset + 1, true);
      dst_selected_start.insert(curve_points.last() + point_offset + 1, false);
      dst_curve_counts[curve_index]++;
      point_offset++;
    }
  }

  if (point_offset == 0) {
    *r_extruded = false;
    return src;
  }
  *r_extruded = true;

  bke::CurvesGeometry dst(dst_to_src_points.size(), src.curves_num());
  BKE_defgroup_copy_list(&dst.vertex_group_names, &src.vertex_group_names);

  /* Setup curve offsets, based on the number of points in each curve. */
  MutableSpan<int> new_curve_offsets = dst.offsets_for_write();
  array_utils::copy(dst_curve_counts.as_span(), new_curve_offsets.drop_back(1));
  offset_indices::accumulate_counts_to_offsets(new_curve_offsets);

  bke::MutableAttributeAccessor dst_attributes = dst.attributes_for_write();

  /* Selection attribute. */
  bke::GSpanAttributeWriter selection = ed::curves::ensure_selection_attribute(
      dst, bke::AttrDomain::Point, bke::AttrType::Bool);
  bke::GSpanAttributeWriter selection_left = ed::curves::ensure_selection_attribute(
      dst, bke::AttrDomain::Point, bke::AttrType::Bool, ".selection_handle_left");
  bke::GSpanAttributeWriter selection_right = ed::curves::ensure_selection_attribute(
      dst, bke::AttrDomain::Point, bke::AttrType::Bool, ".selection_handle_right");
  ed::curves::fill_selection_false(selection.span);
  selection_left.span.copy_from(dst_selected_start.as_span());
  selection_right.span.copy_from(dst_selected_end.as_span());
  selection.finish();
  selection_left.finish();
  selection_right.finish();

  bke::copy_attributes(
      src_attributes, bke::AttrDomain::Curve, bke::AttrDomain::Curve, {}, dst_attributes);

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Point,
                         bke::AttrDomain::Point,
                         bke::attribute_filter_from_skip_ref(
                             {".selection", ".selection_handle_left", ".selection_handle_right"}),
                         dst_to_src_points,
                         dst_attributes);

  Span<float3> src_positions = src.positions();
  MutableSpan<float3> dst_positions = dst.positions_for_write();
  MutableSpan<bool> dst_cyclic = dst.cyclic_for_write();
  const Array<int> dst_point_to_curve_map = dst.point_to_curve_map();
  MutableSpan<int8_t> handle_types_left = dst.handle_types_left_for_write();
  MutableSpan<int8_t> handle_types_right = dst.handle_types_right_for_write();
  for (const int i : dst_to_src_points.index_range()) {
    if (!(dst_selected_end[i] || dst_selected_start[i])) {
      continue;
    }
    const float3 depth_point = src_positions[dst_to_src_points[i]];
    const float2 pos = pen_layer_to_screen(ptd, layer_to_object, depth_point) -
                       ptd.center_of_mass_co + ptd.mouse_co;
    dst_positions[i] = pen_screen_to_layer(ptd, layer_to_world, pos, depth_point);
    handle_types_left[i] = ptd.extrude_handle;
    handle_types_right[i] = ptd.extrude_handle;
    dst_cyclic[dst_point_to_curve_map[i]] = false;
  }

  dst.update_curve_types();
  dst.calculate_bezier_auto_handles();
  if (src.nurbs_has_custom_knots()) {
    IndexMaskMemory memory;
    const VArray<int8_t> curve_types = src.curve_types();
    const VArray<int8_t> knot_modes = dst.nurbs_knots_modes();
    const OffsetIndices<int> dst_points_by_curve = dst.points_by_curve();
    const IndexMask include_curves = IndexMask::from_predicate(
        src.curves_range(), GrainSize(512), memory, [&](const int64_t curve_index) {
          return curve_types[curve_index] == CURVE_TYPE_NURBS &&
                 knot_modes[curve_index] == NURBS_KNOT_MODE_CUSTOM &&
                 points_by_curve[curve_index].size() == dst_points_by_curve[curve_index].size();
        });
    bke::curves::nurbs::update_custom_knot_modes(
        include_curves.complement(dst.curves_range(), memory),
        NURBS_KNOT_MODE_ENDPOINT,
        NURBS_KNOT_MODE_NORMAL,
        dst);
    bke::curves::nurbs::gather_custom_knots(src, include_curves, 0, dst);
  }
  return dst;
}

static void pen_add_single(const PenToolOperation &ptd)
{
  BLI_assert(ptd.grease_pencil->has_active_layer());
  const bke::greasepencil::Layer &layer = *ptd.grease_pencil->get_active_layer();
  bke::greasepencil::Drawing *drawing = ptd.grease_pencil->get_editable_drawing_at(
      layer, ptd.vc.scene->r.cfra);

  bke::CurvesGeometry &curves = drawing->strokes_for_write();

  const float3 depth_point = curves.is_empty() ? float3(0.0f) : curves.positions().last();

  ed::greasepencil::add_single_curve(curves, true);
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  const float4x4 layer_to_world = layer.to_world_space(*ptd.vc.obact);

  curves.positions_for_write().last() = pen_screen_to_layer(
      ptd, layer_to_world, ptd.mouse_co, depth_point);
  curves.curve_types_for_write().last() = CURVE_TYPE_BEZIER;
  curves.handle_types_left_for_write().last() = ptd.extrude_handle;
  curves.handle_types_right_for_write().last() = ptd.extrude_handle;
  drawing->opacities_for_write().last() = 1.0f;
  curves.update_curve_types();

  bke::SpanAttributeWriter<int> material_indexes = attributes.lookup_or_add_for_write_span<int>(
      "material_index",
      bke::AttrDomain::Curve,
      bke::AttributeInitVArray(VArray<int>::from_single(0, curves.curves_num())));

  const int material_index = ptd.vc.obact->actcol - 1;
  material_indexes.span.last() = material_index;
  material_indexes.finish();

  bke::SpanAttributeWriter<float> aspect_ratios = attributes.lookup_or_add_for_write_span<float>(
      "aspect_ratio",
      bke::AttrDomain::Curve,
      bke::AttributeInitVArray(VArray<float>::from_single(0.0f, curves.curves_num())));
  aspect_ratios.span.last() = 1.0f;
  aspect_ratios.finish();

  bke::SpanAttributeWriter<float> u_scales = attributes.lookup_or_add_for_write_span<float>(
      "u_scale",
      bke::AttrDomain::Curve,
      bke::AttributeInitVArray(VArray<float>::from_single(0.0f, curves.curves_num())));
  u_scales.span.last() = 1.0f;
  u_scales.finish();

  MutableSpan<float3> handles_left = curves.handle_positions_left_for_write();
  MutableSpan<float3> handles_right = curves.handle_positions_right_for_write();
  handles_left.last() = pen_screen_to_layer(ptd,
                                            layer_to_world,
                                            ptd.mouse_co -
                                                float2(default_handle_px_distance / 2.0f, 0.0f),
                                            depth_point);
  handles_right.last() = pen_screen_to_layer(ptd,
                                             layer_to_world,
                                             ptd.mouse_co +
                                                 float2(default_handle_px_distance / 2.0f, 0.0f),
                                             depth_point);

  curves.radius_for_write().last() = math::distance(handles_left.last(), handles_right.last()) *
                                     default_radius_factor;

  for (const StringRef selection_attribute_name :
       ed::curves::get_curves_selection_attribute_names(curves))
  {
    bke::GSpanAttributeWriter selection = ed::curves::ensure_selection_attribute(
        curves, bke::AttrDomain::Point, bke::AttrType::Bool, selection_attribute_name);

    ed::curves::fill_selection_true(selection.span,
                                    IndexRange::from_single(curves.points_range().last()));
    selection.finish();
  }

  /* Initialize the rest of the attributes with default values. */
  bke::fill_attribute_range_default(
      attributes,
      bke::AttrDomain::Point,
      bke::attribute_filter_from_skip_ref({"position",
                                           "opacity",
                                           "radius",
                                           "handle_left",
                                           "handle_right",
                                           "handle_type_left",
                                           "handle_type_right",
                                           ".selection",
                                           ".selection_handle_left",
                                           ".selection_handle_right"}),
      curves.points_range().take_back(1));
  bke::fill_attribute_range_default(
      attributes,
      bke::AttrDomain::Curve,
      bke::attribute_filter_from_skip_ref(
          {"curve_type", "material_index", "aspect_ratio", "u_scale"}),
      curves.curves_range().take_back(1));

  drawing->tag_topology_changed();
}

static bke::CurvesGeometry pen_insert_point(const PenToolOperation &ptd,
                                            const bke::CurvesGeometry &src)
{
  const bke::AttributeAccessor src_attributes = src.attributes();
  const OffsetIndices<int> points_by_curve = src.points_by_curve();

  const int old_points_num = src.points_num();

  const int src_point_index = ptd.closest_element.point_index;
  const int dst_point_index = src_point_index + 1;
  const int curve_index = ptd.closest_element.curve_index;

  Vector<int> dst_to_src_points(old_points_num);
  array_utils::fill_index_range(dst_to_src_points.as_mutable_span());

  Vector<int> dst_curve_counts(src.curves_num());
  offset_indices::copy_group_sizes(
      points_by_curve, src.curves_range(), dst_curve_counts.as_mutable_span());

  dst_to_src_points.insert(src_point_index + 1, src_point_index);
  dst_curve_counts[curve_index]++;

  bke::CurvesGeometry dst(dst_to_src_points.size(), src.curves_num());
  BKE_defgroup_copy_list(&dst.vertex_group_names, &src.vertex_group_names);

  /* Setup curve offsets, based on the number of points in each curve. */
  MutableSpan<int> new_curve_offsets = dst.offsets_for_write();
  array_utils::copy(dst_curve_counts.as_span(), new_curve_offsets.drop_back(1));
  offset_indices::accumulate_counts_to_offsets(new_curve_offsets);

  bke::MutableAttributeAccessor dst_attributes = dst.attributes_for_write();

  /* Selection attribute. */
  for (const StringRef selection_attribute_name :
       ed::curves::get_curves_selection_attribute_names(src))
  {
    bke::GSpanAttributeWriter selection_writer = ed::curves::ensure_selection_attribute(
        dst, bke::AttrDomain::Point, bke::AttrType::Bool, selection_attribute_name);
    MutableSpan<bool> selection = selection_writer.span.typed<bool>();
    selection.fill(false);
    selection[dst_point_index] = true;
    selection_writer.finish();
  }

  bke::copy_attributes(
      src_attributes, bke::AttrDomain::Curve, bke::AttrDomain::Curve, {}, dst_attributes);

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Point,
                         bke::AttrDomain::Point,
                         bke::attribute_filter_from_skip_ref(
                             {".selection", ".selection_handle_left", ".selection_handle_right"}),
                         dst_to_src_points,
                         dst_attributes);

  Span<float3> src_positions = src.positions();
  MutableSpan<float3> dst_positions = dst.positions_for_write();
  MutableSpan<int8_t> handle_types_left = dst.handle_types_left_for_write();
  MutableSpan<int8_t> handle_types_right = dst.handle_types_right_for_write();
  const Span<float3> src_handles_left = src.handle_positions_left();
  const Span<float3> src_handles_right = src.handle_positions_right();
  MutableSpan<float3> dst_handles_left = dst.handle_positions_left_for_write();
  MutableSpan<float3> dst_handles_right = dst.handle_positions_right_for_write();
  handle_types_left[dst_point_index] = BEZIER_HANDLE_ALIGN;
  handle_types_right[dst_point_index] = BEZIER_HANDLE_ALIGN;

  const IndexRange points = points_by_curve[curve_index];
  const int src_point_index_2 = (src_point_index + 1 - points.first()) % points.size() +
                                points.first();
  const bke::curves::bezier::Insertion inserted_point = bke::curves::bezier::insert(
      src_positions[src_point_index],
      src_handles_right[src_point_index],
      src_handles_left[src_point_index_2],
      src_positions[src_point_index_2],
      ptd.closest_element.edge_t);

  const int dst_point_index_2 = (dst_point_index - points.first() + 1) % (points.size() + 1) +
                                points.first();

  dst_positions[dst_point_index] = inserted_point.position;
  dst_handles_left[dst_point_index] = inserted_point.left_handle;
  dst_handles_right[dst_point_index] = inserted_point.right_handle;
  dst_handles_right[dst_point_index - 1] = inserted_point.handle_prev;
  dst_handles_left[dst_point_index_2] = inserted_point.handle_next;
  handle_types_right[dst_point_index - 1] = BEZIER_HANDLE_FREE;
  handle_types_left[dst_point_index_2] = BEZIER_HANDLE_FREE;

  dst.update_curve_types();
  dst.calculate_bezier_auto_handles();
  if (src.nurbs_has_custom_knots()) {
    IndexMaskMemory memory;
    const VArray<int8_t> curve_types = src.curve_types();
    const VArray<int8_t> knot_modes = dst.nurbs_knots_modes();
    const OffsetIndices<int> dst_points_by_curve = dst.points_by_curve();
    const IndexMask include_curves = IndexMask::from_predicate(
        src.curves_range(), GrainSize(512), memory, [&](const int64_t curve_index) {
          return curve_types[curve_index] == CURVE_TYPE_NURBS &&
                 knot_modes[curve_index] == NURBS_KNOT_MODE_CUSTOM &&
                 points_by_curve[curve_index].size() == dst_points_by_curve[curve_index].size();
        });
    bke::curves::nurbs::update_custom_knot_modes(
        include_curves.complement(dst.curves_range(), memory),
        NURBS_KNOT_MODE_ENDPOINT,
        NURBS_KNOT_MODE_NORMAL,
        dst);
    bke::curves::nurbs::gather_custom_knots(src, include_curves, 0, dst);
  }
  return dst;
}

static float2 calculate_center_of_mass(const PenToolOperation &ptd)
{
  float2 pos = float2(0.0f, 0.0f);
  int num = 0;

  threading::parallel_for_each(ptd.drawings, [&](const MutableDrawingInfo &info) {
    const bke::CurvesGeometry &curves = info.drawing.strokes();
    const Span<float3> positions = curves.positions();

    IndexMaskMemory memory;
    const IndexMask bezier_points = ed::greasepencil::retrieve_visible_bezier_handle_points(
        *ptd.vc.obact, info.drawing, info.layer_index, memory);

    const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
    const float4x4 layer_to_object = layer.local_transform();

    bezier_points.foreach_index([&](const int64_t point_i) {
      pos += pen_layer_to_screen(ptd, layer_to_object, positions[point_i]);
    });
    num += bezier_points.size();
  });

  if (num == 0) {
    return pos;
  }
  return pos / num;
}

static void pen_status_indicators(bContext *C, wmOperator *op, const PenToolOperation & /*ptd*/)
{
  WorkspaceStatus status(C);
  status.item(IFACE_("Align Angle"), ICON_EVENT_SHIFT);
  status.item(IFACE_("Move Adjacent Handles"), ICON_EVENT_CTRL);
  status.opmodal(IFACE_("Move Entire Point"), op->type, int(PenModal::MoveEntire));
}

/* Invoke handler: Initialize the operator. */
static wmOperatorStatus grease_pencil_pen_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* If in tools region, wait till we get to the main (3D-space)
   * region before allowing drawing to take place. */
  op->flag |= OP_IS_MODAL_CURSOR_REGION;

  wmWindow *win = CTX_wm_window(C);

  /* Set cursor to indicate modal. */
  WM_cursor_modal_set(win, WM_CURSOR_CROSS);

  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_depsgraph_pointer(C));

  /* Allocate new data. */
  PenToolOperation *ptd_pointer = MEM_new<PenToolOperation>(__func__);
  op->customdata = ptd_pointer;
  PenToolOperation &ptd = *ptd_pointer;

  ptd.vc = vc;
  GreasePencil *grease_pencil = static_cast<GreasePencil *>(vc.obact->data);
  ptd.grease_pencil = grease_pencil;
  ptd.projection = ED_view3d_ob_project_mat_get(ptd.vc.rv3d, ptd.vc.obact);

  /* Distance threshold for mouse clicks to affect the spline or its points */
  ptd.mouse_co = float2(event->mval);
  ptd.threshold_distance = ED_view3d_select_dist_px() * selection_distance_factor;
  ptd.threshold_distance_edge = ED_view3d_select_dist_px() * selection_distance_factor_edge;

  ptd.extrude_point = RNA_boolean_get(op->ptr, "extrude_point");
  ptd.delete_point = RNA_boolean_get(op->ptr, "delete_point");
  ptd.insert_point = RNA_boolean_get(op->ptr, "insert_point");
  ptd.move_seg = RNA_boolean_get(op->ptr, "move_segment");
  ptd.select_point = RNA_boolean_get(op->ptr, "select_point");
  ptd.move_point = RNA_boolean_get(op->ptr, "move_point");
  ptd.close_spline = RNA_boolean_get(op->ptr, "close_spline");
  ptd.cycle_handle_type = RNA_boolean_get(op->ptr, "cycle_handle_type");
  ptd.extrude_handle = RNA_enum_get(op->ptr, "extrude_handle");

  const Scene *scene = ptd.vc.scene;

  /* Add a modal handler for this operator. */
  WM_event_add_modal_handler(C, op);

  if (!(ELEM(event->type, LEFTMOUSE) && ELEM(event->val, KM_PRESS, KM_DBL_CLICK))) {
    return OPERATOR_RUNNING_MODAL;
  }

  std::atomic<bool> add_single = ptd.extrude_point;
  std::atomic<bool> changed = false;
  std::atomic<bool> point_added = false;
  std::atomic<bool> point_removed = false;
  ptd.drawings = retrieve_editable_drawings(*scene, *ptd.grease_pencil);
  ptd.center_of_mass_co = calculate_center_of_mass(ptd);

  threading::parallel_for_each(ptd.drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    if (curves.is_empty()) {
      return;
    }

    ptd.closest_element = pen_find_closest_element(
        ptd, info.drawing, info.layer_index, ptd.mouse_co);

    if (ptd.closest_element.element_mode == ElementMode::Edge) {
      add_single.store(false, std::memory_order_relaxed);
      if (ptd.insert_point) {
        curves = pen_insert_point(ptd, curves);
        info.drawing.tag_topology_changed();
        changed.store(true, std::memory_order_relaxed);
      }
      return;
    }

    if (ptd.closest_element.element_mode == ElementMode::None) {
      if (ptd.extrude_point) {
        const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
        const float4x4 layer_to_object = layer.local_transform();
        const float4x4 layer_to_world = layer.to_world_space(*ptd.vc.obact);

        bool extruded = false;
        curves = pen_extrude_curves(ptd, curves, layer_to_object, layer_to_world, &extruded);
        if (!extruded) {
          for (const StringRef selection_attribute_name :
               ed::curves::get_curves_selection_attribute_names(curves))
          {
            bke::GSpanAttributeWriter selection_writer = ed::curves::ensure_selection_attribute(
                curves, bke::AttrDomain::Point, bke::AttrType::Bool, selection_attribute_name);
            MutableSpan<bool> selection = selection_writer.span.typed<bool>();
            selection.fill(false);
            selection_writer.finish();
          }
          return;
        }
        add_single.store(false, std::memory_order_relaxed);
        point_added.store(true, std::memory_order_relaxed);
        info.drawing.tag_topology_changed();

        changed.store(true, std::memory_order_relaxed);
        return;
      }

      return;
    }

    const OffsetIndices points_by_curve = curves.points_by_curve();
    const IndexRange points = points_by_curve[ptd.closest_element.curve_index];

    if (event->val == KM_DBL_CLICK && ptd.cycle_handle_type) {
      const int8_t handle_type =
          curves.handle_types_right_for_write()[ptd.closest_element.point_index];
      /* Cycle to the next type. */
      const int8_t new_handle_type = (handle_type + 1) % 4;

      curves.handle_types_left_for_write()[ptd.closest_element.point_index] = new_handle_type;
      curves.handle_types_right_for_write()[ptd.closest_element.point_index] = new_handle_type;
      curves.update_curve_types();
      curves.calculate_bezier_auto_handles();
      info.drawing.tag_topology_changed();
      add_single.store(false, std::memory_order_relaxed);
    }

    if (ptd.delete_point) {
      curves.remove_points(IndexRange::from_single(ptd.closest_element.point_index), {});
      add_single.store(false, std::memory_order_relaxed);
      point_removed.store(true, std::memory_order_relaxed);
      return;
    }

    for (const StringRef selection_attribute_name :
         ed::curves::get_curves_selection_attribute_names(curves))
    {
      bke::GSpanAttributeWriter selection_writer = ed::curves::ensure_selection_attribute(
          curves, bke::AttrDomain::Point, bke::AttrType::Bool, selection_attribute_name);
      MutableSpan<bool> selection = selection_writer.span.typed<bool>();

      if (ptd.close_spline) {
        if ((ptd.closest_element.point_index == points.first() && selection[points.last()]) ||
            (ptd.closest_element.point_index == points.last() && selection[points.first()]))
        {
          curves.cyclic_for_write()[ptd.closest_element.curve_index] = true;
          curves.calculate_bezier_auto_handles();
          info.drawing.tag_topology_changed();
          add_single.store(false, std::memory_order_relaxed);
        }
      }

      if (event->val != KM_DBL_CLICK && !ptd.delete_point) {
        selection.fill(false);
      }

      if (ptd.select_point) {
        if ((selection_attribute_name == ".selection" &&
             ptd.closest_element.element_mode == ElementMode::Point) ||
            (selection_attribute_name == ".selection_handle_left" &&
             ptd.closest_element.element_mode == ElementMode::HandleLeft) ||
            (selection_attribute_name == ".selection_handle_right" &&
             ptd.closest_element.element_mode == ElementMode::HandleRight))
        {
          selection[ptd.closest_element.point_index] = true;
          add_single.store(false, std::memory_order_relaxed);
        }
      }

      selection_writer.finish();
    }

    changed.store(true, std::memory_order_relaxed);
  });

  if (add_single) {
    pen_add_single(ptd);
    point_added = true;
  }

  pen_status_indicators(C, op, ptd);
  if (changed) {
    grease_pencil_pen_update_view(C, ptd);
  }

  ptd.point_added = point_added;
  ptd.point_removed = point_removed;

  return OPERATOR_RUNNING_MODAL;
}

/* Exit and free memory. */
static void grease_pencil_pen_exit(bContext *C, wmOperator *op)
{
  PenToolOperation *ptd = static_cast<PenToolOperation *>(op->customdata);

  /* Clear status message area. */
  ED_workspace_status_text(C, nullptr);

  WM_cursor_modal_restore(ptd->vc.win);

  grease_pencil_pen_update_view(C, *ptd);

  MEM_delete<PenToolOperation>(ptd);
  /* Clear pointer. */
  op->customdata = nullptr;
}

/* Snaps to the closest diagonal, horizontal or vertical. */
static float2 snap_8_angles(float2 p)
{
  using namespace math;
  /* sin(pi/8) or sin of 22.5 degrees. */
  const float sin225 = 0.3826834323650897717284599840304f;
  return sign(p) * length(p) * normalize(sign(normalize(abs(p)) - sin225) + 1.0f);
}

static void move_segment(const PenToolOperation &ptd,
                         bke::CurvesGeometry &curves,
                         const float4x4 layer_to_world)
{
  const OffsetIndices points_by_curve = curves.points_by_curve();
  MutableSpan<float3> positions = curves.positions_for_write();
  MutableSpan<int8_t> handle_types_left = curves.handle_types_left_for_write();
  MutableSpan<int8_t> handle_types_right = curves.handle_types_right_for_write();
  MutableSpan<float3> handles_left = curves.handle_positions_left_for_write();
  MutableSpan<float3> handles_right = curves.handle_positions_right_for_write();

  const int curve_i = ptd.closest_element.curve_index;
  const IndexRange points = points_by_curve[curve_i];
  const int point_i1 = ptd.closest_element.point_index;
  const int point_i2 = (ptd.closest_element.point_index + 1 - points.first()) % points.size() +
                       points.first();

  const float3 depth_point = positions[point_i1];
  const float3 Pm = pen_screen_to_layer(ptd, layer_to_world, ptd.mouse_co, depth_point);
  const float3 P0 = positions[point_i1];
  const float3 P3 = positions[point_i2];
  const float3 p1 = handles_right[point_i1];
  const float3 p2 = handles_left[point_i2];
  const float3 k2 = p1 - p2;

  const float t = ptd.closest_element.edge_t;
  const float t_sq = t * t;
  const float t_cu = t_sq * t;
  const float one_minus_t = 1.0f - t;
  const float one_minus_t_sq = one_minus_t * one_minus_t;
  const float one_minus_t_cu = one_minus_t_sq * one_minus_t;

  /**
   * Equation of Bezier Curve
   *      => B(t) = (1-t)^3 * P0 + 3(1-t)^2 * t * P1 + 3(1-t) * t^2 * P2 + t^3 * P3
   *
   * Mouse location (Pm) should satisfy this equation.
   * Therefore => Pm = (1-t)^3 * P0 + 3(1-t)^2 * t * P1 + 3(1-t) * t^2 * P2 + t^3 * P3
   *
   * k2 = P1 - P2
   * P2 = P1 - k2
   *
   * Pm - (1-t)^3 * P0 - t^3 * P3 + 3(1-t) * t^2 * k2 = (3(1-t)^2 * t + 3(1-t) * t^2) * P1
   *
   * (Pm - (1-t)^3 * P0 - t^3 * P3 + 3(1-t) * t^2 * k2) / (3(1-t)^2 * t + 3(1-t) * t^2) = P1
   *
   *
   * Another constraint is required to identify P1 and P2.
   * The constraint used is that the vector between P1 and P2 doesn't change.
   * Therefore => P1 - P2 = k2
   *
   * From the two equations => P1 = t(k1 + k2) and P2 = P1 - K2
   */

  const float denom = 3.0f * one_minus_t * t;
  if (denom == 0.0f) {
    return;
  }

  const float3 P1 = (Pm - one_minus_t_cu * P0 - t_cu * P3 + 3.0f * one_minus_t * t_sq * k2) /
                    denom;
  const float3 P2 = P1 - k2;

  handles_right[point_i1] = P1;
  handles_left[point_i2] = P2;
  handle_types_right[point_i1] = BEZIER_HANDLE_FREE;
  handle_types_left[point_i2] = BEZIER_HANDLE_FREE;

  /* Only change `Align`, Keep `Vector` and `Auto` the same. */
  if (handle_types_left[point_i1] == BEZIER_HANDLE_ALIGN) {
    handle_types_left[point_i1] = BEZIER_HANDLE_FREE;
  }
  if (handle_types_right[point_i2] == BEZIER_HANDLE_ALIGN) {
    handle_types_right[point_i2] = BEZIER_HANDLE_FREE;
  }

  curves.calculate_bezier_auto_handles();
}

/* Modal handler: Events handling during interactive part. */
static wmOperatorStatus grease_pencil_pen_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  PenToolOperation &ptd = *reinterpret_cast<PenToolOperation *>(op->customdata);
  Object *object = ptd.vc.obact;

  ptd.mouse_co = float2(event->mval);

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    grease_pencil_pen_exit(C, op);
    return OPERATOR_FINISHED;
  }
  if (ptd.point_removed) {
    grease_pencil_pen_exit(C, op);
    return OPERATOR_FINISHED;
  }

  std::atomic<bool> changed = false;
  ptd.center_of_mass_co = calculate_center_of_mass(ptd);
  threading::parallel_for_each(ptd.drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    MutableSpan<float3> positions = curves.positions_for_write();
    const OffsetIndices points_by_curve = curves.points_by_curve();
    const bke::AttributeAccessor attributes = curves.attributes();
    const Array<int> point_to_curve_map = curves.point_to_curve_map();
    const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
    const float4x4 layer_to_object = layer.local_transform();
    const float4x4 layer_to_world = layer.to_world_space(*ptd.vc.obact);

    MutableSpan<int8_t> handle_types_left = curves.handle_types_left_for_write();
    MutableSpan<int8_t> handle_types_right = curves.handle_types_right_for_write();
    MutableSpan<float3> handles_left = curves.handle_positions_left_for_write();
    MutableSpan<float3> handles_right = curves.handle_positions_right_for_write();

    if (ptd.move_seg && ptd.closest_element.element_mode == ElementMode::Edge) {
      if (ptd.closest_element.layer_index == info.layer_index) {
        move_segment(ptd, curves, layer_to_world);
        info.drawing.tag_topology_changed();
        changed.store(true, std::memory_order_relaxed);
        return;
      }
    }

    IndexMaskMemory memory;
    const IndexMask bezier_points = ed::greasepencil::retrieve_visible_bezier_handle_points(
        *object, info.drawing, info.layer_index, memory);

    if (bezier_points.is_empty()) {
      return;
    }

    const VArray<bool> left_selected = *attributes.lookup_or_default<bool>(
        ".selection_handle_left", bke::AttrDomain::Point, true);
    const VArray<bool> right_selected = *attributes.lookup_or_default<bool>(
        ".selection_handle_right", bke::AttrDomain::Point, true);

    bezier_points.foreach_index(GrainSize(2048), [&](const int64_t point_i) {
      const float3 depth_point = positions[point_i];
      float2 offset = float2(event->xy) - float2(event->prev_xy);

      if (ptd.move_point && !ptd.point_added &&
          !(left_selected[point_i] || right_selected[point_i]))
      {
        positions[point_i] = pen_screen_to_layer(
            ptd,
            layer_to_world,
            pen_layer_to_screen(ptd, layer_to_object, positions[point_i]) + offset,
            depth_point);
        handles_left[point_i] = pen_screen_to_layer(
            ptd,
            layer_to_world,
            pen_layer_to_screen(ptd, layer_to_object, handles_left[point_i]) + offset,
            depth_point);
        handles_right[point_i] = pen_screen_to_layer(
            ptd,
            layer_to_world,
            pen_layer_to_screen(ptd, layer_to_object, handles_right[point_i]) + offset,
            depth_point);
        return;
      }

      if (event->modifier & KM_CTRL) {
        handle_types_left[point_i] = BEZIER_HANDLE_FREE;
        handle_types_right[point_i] = BEZIER_HANDLE_FREE;
        handles_left[point_i] = pen_screen_to_layer(
            ptd,
            layer_to_world,
            pen_layer_to_screen(ptd, layer_to_object, handles_left[point_i]) + offset,
            depth_point);

        const int curve_i = point_to_curve_map[point_i];
        const IndexRange points = points_by_curve[curve_i];
        if (point_i != points.first()) {
          handle_types_left[point_i - 1] = BEZIER_HANDLE_FREE;
          handle_types_right[point_i - 1] = BEZIER_HANDLE_FREE;
          handles_right[point_i - 1] = pen_screen_to_layer(
              ptd,
              layer_to_world,
              pen_layer_to_screen(ptd, layer_to_object, handles_right[point_i - 1]) + offset,
              depth_point);
        }
        return;
      }

      const bool is_left = !right_selected[point_i];
      const float2 center_point = pen_layer_to_screen(ptd, layer_to_object, depth_point);
      offset = ptd.mouse_co - ptd.center_of_mass_co;

      if (event->modifier & KM_SHIFT) {
        offset = snap_8_angles(offset);
      }

      if (ptd.point_added) {
        handle_types_left[point_i] = BEZIER_HANDLE_ALIGN;
        handle_types_right[point_i] = BEZIER_HANDLE_ALIGN;
      }

      if (is_left) {
        if (handle_types_right[point_i] == BEZIER_HANDLE_AUTO) {
          handle_types_right[point_i] = BEZIER_HANDLE_ALIGN;
        }
        handle_types_left[point_i] = handle_types_right[point_i];
        if (handle_types_right[point_i] == BEZIER_HANDLE_VECTOR) {
          handle_types_left[point_i] = BEZIER_HANDLE_FREE;
        }

        handles_left[point_i] = pen_screen_to_layer(
            ptd, layer_to_world, center_point + offset, depth_point);

        if (handle_types_right[point_i] == BEZIER_HANDLE_ALIGN) {
          handles_right[point_i] = 2.0f * depth_point - handles_left[point_i];
        }
      }
      else {
        if (handle_types_left[point_i] == BEZIER_HANDLE_AUTO) {
          handle_types_left[point_i] = BEZIER_HANDLE_ALIGN;
        }
        handle_types_right[point_i] = handle_types_left[point_i];
        if (handle_types_left[point_i] == BEZIER_HANDLE_VECTOR) {
          handle_types_right[point_i] = BEZIER_HANDLE_FREE;
        }

        handles_right[point_i] = pen_screen_to_layer(
            ptd, layer_to_world, center_point + offset, depth_point);
        if (handle_types_left[point_i] == BEZIER_HANDLE_ALIGN) {
          handles_left[point_i] = 2.0f * depth_point - handles_right[point_i];
        }
      }
    });

    curves.calculate_bezier_auto_handles();

    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  });

  pen_status_indicators(C, op, ptd);
  if (changed) {
    grease_pencil_pen_update_view(C, ptd);
  }

  /* Still running... */
  return OPERATOR_RUNNING_MODAL;
}

static void GREASE_PENCIL_OT_pen(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Grease Pencil Pen";
  ot->idname = "GREASE_PENCIL_OT_pen";
  ot->description = "Construct and edit splines";

  /* Callbacks. */
  ot->invoke = grease_pencil_pen_invoke;
  ot->modal = grease_pencil_pen_modal;

  /* Flags. */
  ot->flag = OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_mouse_select(ot);

  RNA_def_boolean(ot->srna,
                  "extrude_point",
                  false,
                  "Extrude Point",
                  "Add a point connected to the last selected point");
  RNA_def_enum(ot->srna,
               "extrude_handle",
               prop_handle_types,
               BEZIER_HANDLE_VECTOR,
               "Extrude Handle Type",
               "Type of the extruded handle");
  RNA_def_boolean(ot->srna, "delete_point", false, "Delete Point", "Delete an existing point");
  RNA_def_boolean(
      ot->srna, "insert_point", false, "Insert Point", "Insert Point into a curve segment");
  RNA_def_boolean(ot->srna, "move_segment", false, "Move Segment", "Delete an existing point");
  RNA_def_boolean(
      ot->srna, "select_point", false, "Select Point", "Select a point or its handles");
  RNA_def_boolean(ot->srna, "move_point", false, "Move Point", "Move a point or its handles");
  RNA_def_boolean(ot->srna,
                  "close_spline",
                  true,
                  "Close Spline",
                  "Make a spline cyclic by clicking endpoints");
  RNA_def_boolean(ot->srna,
                  "cycle_handle_type",
                  false,
                  "Cycle Handle Type",
                  "Cycle between all four handle types");
}

}  // namespace blender::ed::greasepencil

void ED_operatortypes_grease_pencil_pen()
{
  using namespace blender::ed::greasepencil;
  WM_operatortype_append(GREASE_PENCIL_OT_pen);
}

void ED_pentool_modal_keymap(wmKeyConfig *keyconf)
{
  using namespace blender::ed::greasepencil;

  static const EnumPropertyItem modal_items[] = {
      {int(PenModal::FreeAlignToggle),
       "FREE_ALIGN_TOGGLE",
       0,
       "Free-Align Toggle",
       "Move handle of newly added point freely"},
      {int(PenModal::MoveAdjacent),
       "MOVE_ADJACENT",
       0,
       "Move Adjacent Handle",
       "Move the closer handle of the adjacent vertex"},
      {int(PenModal::MoveEntire),
       "MOVE_ENTIRE",
       0,
       "Move Entire Point",
       "Move the entire point using its handles"},
      {int(PenModal::LinkHandles),
       "LINK_HANDLES",
       0,
       "Link Handles",
       "Mirror the movement of one handle onto the other"},
      {int(PenModal::LockAngle),
       "LOCK_ANGLE",
       0,
       "Lock Angle",
       "Move the handle along its current angle"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Grease Pencil Pen Modal Map");

  /* This function is called for each space-type, only needs to add map once. */
  if (keymap && keymap->modal_items) {
    return;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Grease Pencil Pen Modal Map", modal_items);

  WM_modalkeymap_assign(keymap, "GREASE_PENCIL_OT_pen");

  return;
}
