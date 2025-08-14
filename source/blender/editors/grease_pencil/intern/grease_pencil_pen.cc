/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 * Operator for creating bézier splines in Grease Pencil.
 */

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_material.hh"
#include "BKE_report.hh"

#include "BLI_array_utils.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "DEG_depsgraph.hh"

#include "DNA_material_types.h"

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
  /* Move the handles of the adjacent control point. */
  MoveAdjacent = 0,
  /* Move the entire point even if only the handles are selected. */
  MoveEntire = 1,
  /* Snap the handles to multiples of 45 degrees. */
  SnapAngle = 2,
};

enum class ElementMode : int8_t {
  None = 0,
  Point = 1,
  Edge = 2,
  HandleLeft = 3,
  HandleRight = 4,
};

/* Used to scale the default select distance. */
constexpr float selection_distance_factor = 0.9f;
constexpr float selection_distance_factor_edge = 0.5f;

/* Edges are prioritized less than all other types. */
constexpr float selection_edge_priority_factor = 0.1f;

/* Total number of curve handle types. */
constexpr int CURVE_HANDLE_TYPES_NUM = 4;

struct ClosestElement {
  float distance_squared = std::numeric_limits<float>::max();
  ElementMode element_mode;
  int point_index = -1;
  int curve_index = -1;
  float edge_t = -1.0f;
  int drawing_index = -1;

  bool is_closer(const float new_distance_squared,
                 const ElementMode new_element_mode,
                 const float threshold_distance) const
  {
    if (new_distance_squared > threshold_distance * threshold_distance) {
      return false;
    }

    float old_priority = 1.0f;
    float new_priority = 1.0f;

    if (this->element_mode == ElementMode::Edge) {
      if (new_element_mode != ElementMode::Edge) {
        old_priority = selection_edge_priority_factor;
      }
    }
    else {
      if (new_element_mode == ElementMode::Edge) {
        new_priority = selection_edge_priority_factor;
      }
    }

    if (new_distance_squared * old_priority < this->distance_squared * new_priority) {
      return true;
    }

    return false;
  }
};

/* Used when creating a single curve from nothing. */
constexpr float default_handle_px_distance = 16.0f;

/* Snaps to the closest diagonal, horizontal or vertical. */
static float2 snap_8_angles(const float2 &p)
{
  using namespace math;
  /* sin(pi/8) or sin of 22.5 degrees. */
  const float sin225 = sin(AngleRadian::from_degree(22.5f));
  return sign(p) * length(p) * normalize(sign(normalize(abs(p)) - sin225) + 1.0f);
}

struct PenToolOperation {
  ViewContext vc;

  GreasePencil *grease_pencil;
  Vector<MutableDrawingInfo> drawings;

  /* Helper class to project screen space coordinates to 3D. */
  DrawingPlacement placement;

  float threshold_distance;
  float threshold_distance_edge;

  bool extrude_point;
  bool delete_point;
  bool insert_point;
  bool move_seg;
  bool select_point;
  bool move_point;
  bool cycle_handle_type;
  int extrude_handle;
  float radius;

  bool move_entire;
  bool snap_angle;
  bool move_adjacent;

  bool point_added;
  bool point_removed;

  float4x4 projection;
  float2 mouse_co;
  float2 xy;
  float2 prev_xy;
  float2 center_of_mass_co;
  ClosestElement closest_element;

  float2 layer_to_screen(const float4x4 &layer_to_object, const float3 &point) const
  {
    return ED_view3d_project_float_v2_m4(
        vc.region, math::transform_point(layer_to_object, point), projection);
  }

  float3 screen_to_layer(const float4x4 &layer_to_world,
                         const float2 &screen_co,
                         const float3 &depth_point_layer) const
  {
    const float3 depth_point = math::transform_point(layer_to_world, depth_point_layer);
    float3 proj_point;
    ED_view3d_win_to_3d(vc.v3d, vc.region, depth_point, screen_co, proj_point);
    return math::transform_point(math::invert(layer_to_world), proj_point);
  }

  void move_segment() const
  {
    const MutableDrawingInfo &info = this->drawings[this->closest_element.drawing_index];
    const bke::greasepencil::Layer &layer = this->grease_pencil->layer(info.layer_index);
    const float4x4 layer_to_world = layer.to_world_space(*this->vc.obact);
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    MutableSpan<float3> positions = curves.positions_for_write();
    MutableSpan<int8_t> handle_types_left = curves.handle_types_left_for_write();
    MutableSpan<int8_t> handle_types_right = curves.handle_types_right_for_write();
    MutableSpan<float3> handles_left = curves.handle_positions_left_for_write();
    MutableSpan<float3> handles_right = curves.handle_positions_right_for_write();

    const int curve_i = this->closest_element.curve_index;
    const IndexRange points = points_by_curve[curve_i];
    const int point_i1 = this->closest_element.point_index;
    const int point_i2 = (this->closest_element.point_index + 1 - points.first()) % points.size() +
                         points.first();

    const float3 depth_point = positions[point_i1];
    const float3 Pm = this->screen_to_layer(layer_to_world, this->mouse_co, depth_point);
    const float3 P0 = positions[point_i1];
    const float3 P3 = positions[point_i2];
    const float3 p1 = handles_right[point_i1];
    const float3 p2 = handles_left[point_i2];
    const float3 k2 = p1 - p2;

    const float t = this->closest_element.edge_t;
    const float t_sq = t * t;
    const float t_cu = t_sq * t;
    const float one_minus_t = 1.0f - t;
    const float one_minus_t_sq = one_minus_t * one_minus_t;
    const float one_minus_t_cu = one_minus_t_sq * one_minus_t;

    /**
     * Equation of the starting Bezier Curve:
     *      => b(t) = (1-t)^3 * p0 + 3(1-t)^2 * t * p1 + 3(1-t) * t^2 * p2 + t^3 * p3
     *
     * Equation of the moved Bezier Curve:
     *      => B(t) = (1-t)^3 * P0 + 3(1-t)^2 * t * P1 + 3(1-t) * t^2 * P2 + t^3 * P3
     *
     * The moved Bezier curve has four unknowns: P0, P1, P2 and P3
     * We want the end points to stay the same so: P0 = p0 and P3 = p3
     *
     * Mouse location (Pm) should satisfy the equation Pm = B(t).
     * The last constraint used is that the vector between P1 and P2 doesn't change after moving.
     * Therefore: => k2 = p1 - p2 = P1 - P2
     *
     * Using all four equations we can solve for P1 as:
     *      => P1 = (Pm - (1-t)^3 * P0 - t^3 * P3) / (3(1-t) * t) + k2 * t
     * And P2 as:
     *      => P2 = P1 - k2
     */

    const float denom = 3.0f * one_minus_t * t;
    if (denom == 0.0f) {
      return;
    }

    const float3 P1 = (Pm - one_minus_t_cu * P0 - t_cu * P3) / denom + k2 * t;
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
    info.drawing.tag_topology_changed();
  }

  bool move_handles_in_drawing(const MutableDrawingInfo &info) const
  {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    MutableSpan<float3> positions = curves.positions_for_write();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const bke::AttributeAccessor attributes = curves.attributes();
    const Array<int> point_to_curve_map = curves.point_to_curve_map();
    const bke::greasepencil::Layer &layer = this->grease_pencil->layer(info.layer_index);
    const float4x4 layer_to_object = layer.local_transform();
    const float4x4 layer_to_world = layer.to_world_space(*this->vc.obact);

    MutableSpan<int8_t> handle_types_left = curves.handle_types_left_for_write();
    MutableSpan<int8_t> handle_types_right = curves.handle_types_right_for_write();
    MutableSpan<float3> handles_left = curves.handle_positions_left_for_write();
    MutableSpan<float3> handles_right = curves.handle_positions_right_for_write();

    IndexMaskMemory memory;
    const IndexMask bezier_points = ed::greasepencil::retrieve_visible_bezier_handle_points(
        *this->vc.obact,
        info.drawing,
        info.layer_index,
        this->vc.v3d->overlay.handle_display,
        memory);
    if (bezier_points.is_empty()) {
      return false;
    }

    const VArray<bool> left_selected = *attributes.lookup_or_default<bool>(
        ".selection_handle_left", bke::AttrDomain::Point, true);
    const VArray<bool> right_selected = *attributes.lookup_or_default<bool>(
        ".selection_handle_right", bke::AttrDomain::Point, true);

    bezier_points.foreach_index(GrainSize(2048), [&](const int64_t point_i) {
      const float3 depth_point = positions[point_i];
      float2 offset = this->xy - this->prev_xy;

      if ((this->move_point && !this->point_added &&
           !(left_selected[point_i] || right_selected[point_i])) ||
          this->move_entire)
      {
        const float2 pos = this->layer_to_screen(layer_to_object, positions[point_i]);
        const float2 pos_left = this->layer_to_screen(layer_to_object, handles_left[point_i]);
        const float2 pos_right = this->layer_to_screen(layer_to_object, handles_right[point_i]);
        positions[point_i] = this->screen_to_layer(layer_to_world, pos + offset, depth_point);
        handles_left[point_i] = this->screen_to_layer(
            layer_to_world, pos_left + offset, depth_point);
        handles_right[point_i] = this->screen_to_layer(
            layer_to_world, pos_right + offset, depth_point);
        return;
      }

      if (this->move_adjacent) {
        handle_types_left[point_i] = BEZIER_HANDLE_FREE;
        handle_types_right[point_i] = BEZIER_HANDLE_FREE;
        const float2 pos_left = this->layer_to_screen(layer_to_object, handles_left[point_i]);
        handles_left[point_i] = this->screen_to_layer(
            layer_to_world, pos_left + offset, depth_point);

        const int curve_i = point_to_curve_map[point_i];
        const IndexRange points = points_by_curve[curve_i];
        if (point_i != points.first()) {
          const float2 pos_right = this->layer_to_screen(layer_to_object,
                                                         handles_right[point_i - 1]);
          handle_types_left[point_i - 1] = BEZIER_HANDLE_FREE;
          handle_types_right[point_i - 1] = BEZIER_HANDLE_FREE;
          handles_right[point_i - 1] = this->screen_to_layer(
              layer_to_world, pos_right + offset, depth_point);
        }
        return;
      }

      const bool is_left = !right_selected[point_i];
      const float2 center_point = this->layer_to_screen(layer_to_object, depth_point);
      offset = this->mouse_co - this->center_of_mass_co;

      if (this->snap_angle) {
        offset = snap_8_angles(offset);
      }

      if (this->point_added) {
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

        if (this->point_added) {
          handles_left[point_i] = this->placement.project(center_point + offset);
        }
        else {
          handles_left[point_i] = this->screen_to_layer(
              layer_to_world, center_point + offset, depth_point);
        }

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

        if (this->point_added) {
          handles_right[point_i] = this->placement.project(center_point + offset);
        }
        else {
          handles_right[point_i] = this->screen_to_layer(
              layer_to_world, center_point + offset, depth_point);
        }

        if (handle_types_left[point_i] == BEZIER_HANDLE_ALIGN) {
          handles_left[point_i] = 2.0f * depth_point - handles_right[point_i];
        }
      }
    });

    curves.calculate_bezier_auto_handles();

    info.drawing.tag_topology_changed();
    return true;
  }

  std::optional<bke::CurvesGeometry> extrude_curves(const bke::greasepencil::Drawing &drawing,
                                                    const int layer_index,
                                                    const float4x4 &layer_to_object) const
  {
    const bke::CurvesGeometry &src = drawing.strokes();
    const bke::AttributeAccessor src_attributes = src.attributes();
    const OffsetIndices<int> points_by_curve = src.points_by_curve();
    const VArray<bool> &src_cyclic = src.cyclic();
    const VArray<int8_t> types = src.curve_types();
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
    Vector<bool> dst_selected_center(old_points_num, false);
    Vector<bool> dst_selected_end(old_points_num, false);

    Vector<int> dst_curve_counts(src.curves_num());
    offset_indices::copy_group_sizes(
        points_by_curve, src.curves_range(), dst_curve_counts.as_mutable_span());

    IndexMaskMemory memory;
    const IndexMask editable_curves = ed::greasepencil::retrieve_editable_strokes(
        *this->vc.obact, drawing, layer_index, memory);

    /* Point offset keeps track of the points inserted. */
    int point_offset = 0;
    editable_curves.foreach_index([&](const int curve_index) {
      const IndexRange curve_points = points_by_curve[curve_index];
      /* Skip cyclic curves unless they only have one point. */
      if (src_cyclic[curve_index] && curve_points.size() != 1) {
        return;
      }
      const bool is_bezier = types[curve_index] == CURVE_TYPE_BEZIER;

      bool first_selected = point_selection[curve_points.first()];
      if (is_bezier) {
        first_selected |= left_selected[curve_points.first()];
        first_selected |= right_selected[curve_points.first()];
      }

      bool last_selected = point_selection[curve_points.last()];
      if (is_bezier) {
        last_selected |= left_selected[curve_points.last()];
        last_selected |= right_selected[curve_points.last()];
      }

      if (first_selected) {
        if (curve_points.size() != 1) {
          /* Start-point extruded, we insert a new point at the beginning of the curve. */
          dst_to_src_points.insert(curve_points.first() + point_offset, curve_points.first());
          dst_selected_start.insert(curve_points.first() + point_offset, true);
          dst_selected_center.insert(curve_points.first() + point_offset, !is_bezier);
          dst_selected_end.insert(curve_points.first() + point_offset, false);
          dst_curve_counts[curve_index]++;
          point_offset++;
        }
      }

      if (last_selected) {
        /* End-point extruded, we insert a new point at the end of the curve. */
        dst_to_src_points.insert(curve_points.last() + point_offset + 1, curve_points.last());
        dst_selected_end.insert(curve_points.last() + point_offset + 1, true);
        dst_selected_center.insert(curve_points.last() + point_offset + 1, !is_bezier);
        dst_selected_start.insert(curve_points.last() + point_offset + 1, false);
        dst_curve_counts[curve_index]++;
        point_offset++;
      }
    });

    if (point_offset == 0) {
      return std::nullopt;
    }

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
    selection_left.span.copy_from(dst_selected_start.as_span());
    selection.span.copy_from(dst_selected_center.as_span());
    selection_right.span.copy_from(dst_selected_end.as_span());
    selection_left.finish();
    selection.finish();
    selection_right.finish();

    bke::copy_attributes(
        src_attributes, bke::AttrDomain::Curve, bke::AttrDomain::Curve, {}, dst_attributes);

    bke::gather_attributes(
        src_attributes,
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
    MutableSpan<float> radius = dst.radius_for_write();
    for (const int i : dst_to_src_points.index_range()) {
      if (!(dst_selected_end[i] || dst_selected_start[i])) {
        continue;
      }
      const float3 depth_point = src_positions[dst_to_src_points[i]];
      const float2 pos = this->layer_to_screen(layer_to_object, depth_point) -
                         this->center_of_mass_co + this->mouse_co;
      dst_positions[i] = this->placement.project(pos);
      handle_types_left[i] = this->extrude_handle;
      handle_types_right[i] = this->extrude_handle;
      radius[i] = this->radius;
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

  void insert_point_to_curve(bke::CurvesGeometry &src) const
  {
    const bke::AttributeAccessor src_attributes = src.attributes();
    const OffsetIndices<int> points_by_curve = src.points_by_curve();
    const int old_points_num = src.points_num();
    const int src_point_index = this->closest_element.point_index;
    const int dst_point_index = src_point_index + 1;
    const int curve_index = this->closest_element.curve_index;
    const IndexRange points = points_by_curve[curve_index];
    const int src_point_index_2 = (src_point_index + 1 - points.first()) % points.size() +
                                  points.first();
    const int dst_point_index_2 = (dst_point_index - points.first() + 1) % (points.size() + 1) +
                                  points.first();

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
    bke::gather_attributes(
        src_attributes,
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

    const bke::curves::bezier::Insertion inserted_point = bke::curves::bezier::insert(
        src_positions[src_point_index],
        src_handles_right[src_point_index],
        src_handles_left[src_point_index_2],
        src_positions[src_point_index_2],
        this->closest_element.edge_t);

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

    src = std::move(dst);
  }

  void add_single_point_and_curve() const
  {
    bke::greasepencil::Layer &layer = *this->grease_pencil->get_active_layer();
    bke::greasepencil::Drawing *drawing = this->grease_pencil->get_editable_drawing_at(
        layer, this->vc.scene->r.cfra);
    bke::CurvesGeometry &curves = drawing->strokes_for_write();
    const float3 depth_point = curves.is_empty() ? float3(0.0f) : curves.positions().last();

    ed::greasepencil::add_single_curve(curves, true);
    bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
    const float4x4 layer_to_world = layer.to_world_space(*this->vc.obact);

    Set<std::string> curve_attributes_to_skip;

    curves.positions_for_write().last() = this->placement.project(this->mouse_co);
    curves.curve_types_for_write().last() = CURVE_TYPE_BEZIER;
    curve_attributes_to_skip.add("curve_type");
    curves.handle_types_left_for_write().last() = this->extrude_handle;
    curves.handle_types_right_for_write().last() = this->extrude_handle;
    drawing->opacities_for_write().last() = 1.0f;
    curves.update_curve_types();

    const int material_index = this->vc.obact->actcol - 1;
    if (material_index != 0) {
      bke::SpanAttributeWriter<int> material_indexes =
          attributes.lookup_or_add_for_write_span<int>(
              "material_index",
              bke::AttrDomain::Curve,
              bke::AttributeInitVArray(VArray<int>::from_single(0, curves.curves_num())));
      material_indexes.span.last() = material_index;
      material_indexes.finish();
      curve_attributes_to_skip.add("material_index");
    }

    bke::SpanAttributeWriter<float> aspect_ratios = attributes.lookup_or_add_for_write_span<float>(
        "aspect_ratio",
        bke::AttrDomain::Curve,
        bke::AttributeInitVArray(VArray<float>::from_single(0.0f, curves.curves_num())));
    aspect_ratios.span.last() = 1.0f;
    aspect_ratios.finish();
    curve_attributes_to_skip.add("aspect_ratio");

    bke::SpanAttributeWriter<float> u_scales = attributes.lookup_or_add_for_write_span<float>(
        "u_scale",
        bke::AttrDomain::Curve,
        bke::AttributeInitVArray(VArray<float>::from_single(0.0f, curves.curves_num())));
    u_scales.span.last() = 1.0f;
    u_scales.finish();
    curve_attributes_to_skip.add("u_scale");

    MutableSpan<float3> handles_left = curves.handle_positions_left_for_write();
    MutableSpan<float3> handles_right = curves.handle_positions_right_for_write();
    handles_left.last() = this->screen_to_layer(
        layer_to_world,
        this->mouse_co - float2(default_handle_px_distance / 2.0f, 0.0f),
        depth_point);
    handles_right.last() = this->screen_to_layer(
        layer_to_world,
        this->mouse_co + float2(default_handle_px_distance / 2.0f, 0.0f),
        depth_point);
    curves.radius_for_write().last() = this->radius;

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
        bke::attribute_filter_from_skip_ref(curve_attributes_to_skip),
        curves.curves_range().take_back(1));

    drawing->tag_topology_changed();
  }
};

static void grease_pencil_pen_update_view(bContext *C, PenToolOperation &ptd)
{
  GreasePencil *grease_pencil = ptd.grease_pencil;

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, grease_pencil);

  ED_region_tag_redraw(ptd.vc.region);
}

/* Will check if the point or handle is closer than the existing element. */
static void pen_find_closest_point_or_handle(const PenToolOperation &ptd,
                                             const bke::greasepencil::Drawing &drawing,
                                             const int layer_index,
                                             const int drawing_index,
                                             const float2 &mouse_co,
                                             ClosestElement &r_closest_element)
{
  const bke::CurvesGeometry &curves = drawing.strokes();
  const Span<float3> positions = curves.positions();

  const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(layer_index);
  const float4x4 layer_to_object = layer.local_transform();
  const Array<int> point_to_curve_map = curves.point_to_curve_map();

  IndexMaskMemory memory;
  const IndexMask editable_points = ed::greasepencil::retrieve_editable_points(
      *ptd.vc.obact, drawing, layer_index, memory);
  editable_points.foreach_index([&](const int point_i) {
    const float2 pos_proj = ptd.layer_to_screen(layer_to_object, positions[point_i]);
    const float distance_squared = math::distance_squared(pos_proj, mouse_co);

    /* Save the closest point. */
    if (r_closest_element.is_closer(distance_squared, ElementMode::Point, ptd.threshold_distance))
    {
      r_closest_element.curve_index = point_to_curve_map[point_i];
      r_closest_element.point_index = point_i;
      r_closest_element.element_mode = ElementMode::Point;
      r_closest_element.distance_squared = distance_squared;
      r_closest_element.drawing_index = drawing_index;
    }
  });

  const Span<float3> handle_left = curves.handle_positions_left();
  const Span<float3> handle_right = curves.handle_positions_right();
  const IndexMask bezier_points = ed::greasepencil::retrieve_visible_bezier_handle_points(
      *ptd.vc.obact, drawing, layer_index, ptd.vc.v3d->overlay.handle_display, memory);

  bezier_points.foreach_index([&](const int point_i) {
    const float2 pos_proj = ptd.layer_to_screen(layer_to_object, handle_left[point_i]);
    const float distance_squared = math::distance_squared(pos_proj, mouse_co);

    /* Save the closest point. */
    if (r_closest_element.is_closer(
            distance_squared, ElementMode::HandleLeft, ptd.threshold_distance))
    {
      r_closest_element.curve_index = point_to_curve_map[point_i];
      r_closest_element.point_index = point_i;
      r_closest_element.element_mode = ElementMode::HandleLeft;
      r_closest_element.distance_squared = distance_squared;
      r_closest_element.drawing_index = drawing_index;
    }
  });

  bezier_points.foreach_index([&](const int point_i) {
    const float2 pos_proj = ptd.layer_to_screen(layer_to_object, handle_right[point_i]);
    const float distance_squared = math::distance_squared(pos_proj, mouse_co);

    /* Save the closest point. */
    if (r_closest_element.is_closer(
            distance_squared, ElementMode::HandleRight, ptd.threshold_distance))
    {
      r_closest_element.curve_index = point_to_curve_map[point_i];
      r_closest_element.point_index = point_i;
      r_closest_element.element_mode = ElementMode::HandleRight;
      r_closest_element.distance_squared = distance_squared;
      r_closest_element.drawing_index = drawing_index;
    }
  });
}

static float2 line_segment_closest_point(const float2 &pos_1,
                                         const float2 &pos_2,
                                         const float2 &pos,
                                         float &r_local_t)
{
  const float2 dif_m = pos - pos_1;
  const float2 dif_l = pos_2 - pos_1;
  const float d = math::dot(dif_m, dif_l);
  const float l2 = math::dot(dif_l, dif_l);
  const float t = math::clamp(d / l2, 0.0f, 1.0f);
  r_local_t = t;
  return dif_l * t + pos_1;
}

/* Will check if the edge point is closer than the existing element. */
static void pen_find_closest_edge_point(const PenToolOperation &ptd,
                                        const bke::greasepencil::Drawing &drawing,
                                        const int layer_index,
                                        const int drawing_index,
                                        const float2 &mouse_co,
                                        ClosestElement &r_closest_element)
{
  const bke::CurvesGeometry &curves = drawing.strokes();
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const OffsetIndices<int> evaluated_points_by_curve = curves.evaluated_points_by_curve();
  const Span<float3> positions = curves.positions();
  const Span<float3> evaluated_positions = curves.evaluated_positions();
  const VArray<bool> cyclic = curves.cyclic();
  const VArray<int8_t> types = curves.curve_types();

  IndexMaskMemory memory;
  const IndexMask editable_curves = ed::greasepencil::retrieve_editable_strokes(
      *ptd.vc.obact, drawing, layer_index, memory);
  const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(layer_index);
  const float4x4 layer_to_object = layer.local_transform();

  editable_curves.foreach_index([&](const int curve_i) {
    const IndexRange src_points = points_by_curve[curve_i];
    const IndexRange eval_points = evaluated_points_by_curve[curve_i];

    for (const int src_i : src_points.index_range().drop_back(cyclic[curve_i] ? 0 : 1)) {
      if (types[curve_i] != CURVE_TYPE_BEZIER) {
        const int src_i_1 = src_i + src_points.first();
        const int src_i_2 = (src_i + 1) % src_points.size() + src_points.first();
        const float2 pos_1_proj = ptd.layer_to_screen(layer_to_object, positions[src_i_1]);
        const float2 pos_2_proj = ptd.layer_to_screen(layer_to_object, positions[src_i_2]);
        float local_t;
        const float2 closest_pos = line_segment_closest_point(
            pos_1_proj, pos_2_proj, mouse_co, local_t);

        const float distance_squared = math::distance_squared(closest_pos, mouse_co);
        const float t = local_t;

        /* Save the closest point. */
        if (r_closest_element.is_closer(
                distance_squared, ElementMode::Edge, ptd.threshold_distance_edge))
        {
          r_closest_element.point_index = src_points.first() + src_i;
          r_closest_element.edge_t = t;
          r_closest_element.element_mode = ElementMode::Edge;
          r_closest_element.curve_index = curve_i;
          r_closest_element.distance_squared = distance_squared;
          r_closest_element.drawing_index = drawing_index;
        }
      }
      else {
        const Span<int> offsets = curves.bezier_evaluated_offsets_for_curve(curve_i);
        const IndexRange eval_range = IndexRange::from_begin_end_inclusive(offsets[src_i],
                                                                           offsets[src_i + 1])
                                          .shift(eval_points.first());
        const int point_num = eval_range.size() - 1;

        for (const int eval_i : IndexRange(point_num)) {
          const int eval_point_i_1 = eval_range.first() + eval_i;
          const int eval_point_i_2 = (eval_range.first() + eval_i + 1 - eval_points.first()) %
                                         eval_points.size() +
                                     eval_points.first();
          const float2 pos_1_proj = ptd.layer_to_screen(layer_to_object,
                                                        evaluated_positions[eval_point_i_1]);
          const float2 pos_2_proj = ptd.layer_to_screen(layer_to_object,
                                                        evaluated_positions[eval_point_i_2]);
          float local_t;
          const float2 closest_pos = line_segment_closest_point(
              pos_1_proj, pos_2_proj, mouse_co, local_t);

          const float distance_squared = math::distance_squared(closest_pos, mouse_co);
          const float t = (eval_i + local_t) / float(point_num);

          /* Save the closest point. */
          if (r_closest_element.is_closer(
                  distance_squared, ElementMode::Edge, ptd.threshold_distance_edge))
          {
            r_closest_element.point_index = src_points.first() + src_i;
            r_closest_element.element_mode = ElementMode::Edge;
            r_closest_element.edge_t = t;
            r_closest_element.curve_index = curve_i;
            r_closest_element.distance_squared = distance_squared;
            r_closest_element.drawing_index = drawing_index;
          }
        }
      }
    }
  });
}

static ClosestElement pen_find_closest_element(const PenToolOperation &ptd, const float2 &mouse_co)
{
  ClosestElement closest_element;
  closest_element.element_mode = ElementMode::None;

  for (const int drawing_index : ptd.drawings.index_range()) {
    const MutableDrawingInfo &info = ptd.drawings[drawing_index];

    pen_find_closest_point_or_handle(
        ptd, info.drawing, info.layer_index, drawing_index, mouse_co, closest_element);
    pen_find_closest_edge_point(
        ptd, info.drawing, info.layer_index, drawing_index, mouse_co, closest_element);
  }
  return closest_element;
}

/**
 * Will return if a new curve can be created, and will report any errors.
 */
static bool pen_report_new_curve_errors(const PenToolOperation &ptd, wmOperator *op)
{
  if (!ptd.grease_pencil->has_active_layer()) {
    BKE_report(op->reports, RPT_ERROR, "No active Grease Pencil layer");
    return false;
  }

  bke::greasepencil::Layer &layer = *ptd.grease_pencil->get_active_layer();
  if (!layer.is_editable()) {
    BKE_report(op->reports, RPT_ERROR, "Active layer is locked or hidden");
    return false;
  }

  const int material_index = ptd.vc.obact->actcol - 1;
  Material *material = BKE_object_material_get(ptd.vc.obact, material_index + 1);
  /* The editable materials are unlocked and not hidden. */
  if (material != nullptr && material->gp_style != nullptr &&
      ((material->gp_style->flag & GP_MATERIAL_LOCKED) != 0 ||
       (material->gp_style->flag & GP_MATERIAL_HIDE) != 0))
  {
    BKE_report(op->reports, RPT_ERROR, "Active Material is locked or hidden");
    return false;
  }

  /* Ensure a drawing at the current keyframe. */
  bool inserted_keyframe = false;
  if (!ed::greasepencil::ensure_active_keyframe(
          *ptd.vc.scene, *ptd.grease_pencil, layer, false, inserted_keyframe))
  {
    BKE_report(op->reports, RPT_ERROR, "No Grease Pencil frame to draw on");
    return false;
  }

  return true;
}

static float2 calculate_center_of_mass(const PenToolOperation &ptd, const bool ends_only)
{
  float2 pos = float2(0.0f, 0.0f);
  int num = 0;

  for (const int drawing_index : ptd.drawings.index_range()) {
    const MutableDrawingInfo &info = ptd.drawings[drawing_index];
    const bke::CurvesGeometry &curves = info.drawing.strokes();
    const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
    const float4x4 layer_to_object = layer.local_transform();
    const Span<float3> positions = curves.positions();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const Array<int> point_to_curve_map = curves.point_to_curve_map();
    const VArray<bool> &cyclic = curves.cyclic();

    IndexMaskMemory memory;
    const IndexMask selection = ed::greasepencil::retrieve_editable_and_selected_points(
        *ptd.vc.obact, info.drawing, info.layer_index, memory);
    const IndexMask bezier_points = ed::greasepencil::retrieve_visible_bezier_handle_points(
        *ptd.vc.obact, info.drawing, info.layer_index, ptd.vc.v3d->overlay.handle_display, memory);
    const IndexMask all_points = IndexMask::from_union(selection, bezier_points, memory);

    all_points.foreach_index([&](const int64_t point_i) {
      if (ends_only) {
        const int curve_i = point_to_curve_map[point_i];
        const IndexRange points = points_by_curve[curve_i];

        /* Skip cyclic curves unless they only have one point. */
        if (cyclic[curve_i] && points.size() != 1) {
          return;
        }

        if (!(point_i == points.last() || point_i == points.first())) {
          return;
        }
      }
      pos += ptd.layer_to_screen(layer_to_object, positions[point_i]);
      num++;
    });
  }

  if (num == 0) {
    return pos;
  }
  return pos / num;
}

static void pen_status_indicators(bContext *C, wmOperator *op, const PenToolOperation & /*ptd*/)
{
  WorkspaceStatus status(C);
  status.opmodal(IFACE_("Snap Angle"), op->type, int(PenModal::SnapAngle));
  status.opmodal(IFACE_("Move Adjacent Handles"), op->type, int(PenModal::MoveAdjacent));
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
  View3D *view3d = CTX_wm_view3d(C);

  /* Initialize helper class for projecting screen space coordinates. */
  DrawingPlacement placement = DrawingPlacement(
      *vc.scene, *vc.region, *view3d, *vc.obact, grease_pencil->get_active_layer());
  if (placement.use_project_to_surface()) {
    placement.cache_viewport_depths(CTX_data_depsgraph_pointer(C), vc.region, view3d);
  }
  else if (placement.use_project_to_stroke()) {
    placement.cache_viewport_depths(CTX_data_depsgraph_pointer(C), vc.region, view3d);
  }

  ptd.placement = placement;

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
  ptd.cycle_handle_type = RNA_boolean_get(op->ptr, "cycle_handle_type");
  ptd.extrude_handle = RNA_enum_get(op->ptr, "extrude_handle");
  ptd.radius = RNA_float_get(op->ptr, "radius");

  ptd.move_entire = false;
  ptd.snap_angle = false;

  /* Add a modal handler for this operator. */
  WM_event_add_modal_handler(C, op);

  if (!(ELEM(event->type, LEFTMOUSE) && ELEM(event->val, KM_PRESS, KM_DBL_CLICK))) {
    return OPERATOR_RUNNING_MODAL;
  }

  std::atomic<bool> add_single = ptd.extrude_point;
  std::atomic<bool> changed = false;
  std::atomic<bool> point_added = false;
  std::atomic<bool> point_removed = false;
  ptd.drawings = retrieve_editable_drawings(*ptd.vc.scene, *ptd.grease_pencil);
  ptd.center_of_mass_co = calculate_center_of_mass(ptd, true);
  ptd.closest_element = pen_find_closest_element(ptd, ptd.mouse_co);

  threading::parallel_for_each(ptd.drawings, [&](const MutableDrawingInfo &info) {
    const int drawing_index = (&info - ptd.drawings.data());
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    if (curves.is_empty()) {
      return;
    }

    if (ptd.closest_element.element_mode == ElementMode::Edge) {
      add_single.store(false, std::memory_order_relaxed);
      if (ptd.insert_point) {
        ptd.insert_point_to_curve(curves);
        info.drawing.tag_topology_changed();
        changed.store(true, std::memory_order_relaxed);
      }
      return;
    }

    if (ptd.closest_element.element_mode == ElementMode::None) {
      if (ptd.extrude_point) {
        const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
        const float4x4 layer_to_object = layer.local_transform();

        const std::optional<bke::CurvesGeometry> result = ptd.extrude_curves(
            info.drawing, info.layer_index, layer_to_object);

        if (result) {
          curves = *result;
        }
        else {
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

    if (drawing_index != ptd.closest_element.drawing_index) {
      if (event->val != KM_DBL_CLICK && !ptd.delete_point) {
        for (const StringRef selection_attribute_name :
             ed::curves::get_curves_selection_attribute_names(curves))
        {
          bke::GSpanAttributeWriter selection_writer = ed::curves::ensure_selection_attribute(
              curves, bke::AttrDomain::Point, bke::AttrType::Bool, selection_attribute_name);
          MutableSpan<bool> selection = selection_writer.span.typed<bool>();
          selection.fill(false);
          selection_writer.finish();
        }
      }

      return;
    }

    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const IndexRange points = points_by_curve[ptd.closest_element.curve_index];

    if (event->val == KM_DBL_CLICK && ptd.cycle_handle_type) {
      const int8_t handle_type = curves.handle_types_right()[ptd.closest_element.point_index];
      /* Cycle to the next type. */
      const int8_t new_handle_type = (handle_type + 1) % CURVE_HANDLE_TYPES_NUM;

      curves.handle_types_left_for_write()[ptd.closest_element.point_index] = new_handle_type;
      curves.handle_types_right_for_write()[ptd.closest_element.point_index] = new_handle_type;
      curves.calculate_bezier_auto_handles();
      info.drawing.tag_topology_changed();
      add_single.store(false, std::memory_order_relaxed);
    }

    if (ptd.delete_point) {
      curves.remove_points(IndexRange::from_single(ptd.closest_element.point_index), {});
      add_single.store(false, std::memory_order_relaxed);
      point_removed.store(true, std::memory_order_relaxed);
      info.drawing.tag_topology_changed();
      return;
    }

    for (const StringRef selection_attribute_name :
         ed::curves::get_curves_selection_attribute_names(curves))
    {
      bke::GSpanAttributeWriter selection_writer = ed::curves::ensure_selection_attribute(
          curves, bke::AttrDomain::Point, bke::AttrType::Bool, selection_attribute_name);
      MutableSpan<bool> selection = selection_writer.span.typed<bool>();

      /* Close the curve by selecting the other end point. */
      if ((ptd.closest_element.point_index == points.first() && selection[points.last()]) ||
          (ptd.closest_element.point_index == points.last() && selection[points.first()]))
      {
        curves.cyclic_for_write()[ptd.closest_element.curve_index] = true;
        curves.calculate_bezier_auto_handles();
        info.drawing.tag_topology_changed();
        add_single.store(false, std::memory_order_relaxed);
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
    const bool successful = pen_report_new_curve_errors(ptd, op);
    if (successful) {
      ptd.add_single_point_and_curve();
      changed.store(true, std::memory_order_relaxed);
      point_added = true;
    }
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

  MEM_delete(ptd);
  /* Clear pointer. */
  op->customdata = nullptr;
}

/* Modal handler: Events handling during interactive part. */
static wmOperatorStatus grease_pencil_pen_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  PenToolOperation &ptd = *reinterpret_cast<PenToolOperation *>(op->customdata);

  ptd.mouse_co = float2(event->mval);
  ptd.xy = float2(event->xy);
  ptd.prev_xy = float2(event->prev_xy);

  if (event->type == EVENT_NONE) {
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    grease_pencil_pen_exit(C, op);
    return OPERATOR_FINISHED;
  }
  if (ptd.point_removed) {
    grease_pencil_pen_exit(C, op);
    return OPERATOR_FINISHED;
  }

  if (event->type == EVT_MODAL_MAP) {
    if (event->val == int(PenModal::MoveEntire)) {
      ptd.move_entire = !ptd.move_entire;
    }
    else if (event->val == int(PenModal::SnapAngle)) {
      ptd.snap_angle = !ptd.snap_angle;
    }
    else if (event->val == int(PenModal::MoveAdjacent)) {
      ptd.move_adjacent = !ptd.move_adjacent;
    }
  }

  std::atomic<bool> changed = false;
  ptd.center_of_mass_co = calculate_center_of_mass(ptd, false);

  if (ptd.move_seg && ptd.closest_element.element_mode == ElementMode::Edge) {
    ptd.move_segment();
    changed.store(true, std::memory_order_relaxed);
  }
  else {
    threading::parallel_for_each(ptd.drawings, [&](const MutableDrawingInfo &info) {
      if (ptd.move_handles_in_drawing(info)) {
        changed.store(true, std::memory_order_relaxed);
      }
    });
  }

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
                  "cycle_handle_type",
                  false,
                  "Cycle Handle Type",
                  "Cycle between all four handle types");
  RNA_def_float_distance(ot->srna, "radius", 0.01f, 0.0f, FLT_MAX, "Radius", "", 0.0f, 10.0f);
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
      {int(PenModal::SnapAngle),
       "SNAP_ANGLE",
       0,
       "Snap Angle",
       "Snap the handle angle to 45 degrees"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Pen Tool Modal Map");

  /* This function is called for each space-type, only needs to add map once. */
  if (keymap && keymap->modal_items) {
    return;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Pen Tool Modal Map", modal_items);
  WM_modalkeymap_assign(keymap, "GREASE_PENCIL_OT_pen");
}
