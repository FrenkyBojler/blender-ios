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

#include "BLI_array_utils.hh"

namespace blender::ed::greasepencil {

static const EnumPropertyItem prop_handle_types[] = {
    {BEZIER_HANDLE_AUTO, "AUTO", 0, "Auto", ""},
    {BEZIER_HANDLE_VECTOR, "VECTOR", 0, "Vector", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class CloseMethod : int8_t {
  Off = 0,
  OnPress = 1,
  OnClick = 2,
};

static const EnumPropertyItem prop_close_method[] = {
    {int(CloseMethod::Off), "OFF", 0, "None", ""},
    {int(CloseMethod::OnPress),
     "ON_PRESS",
     0,
     "On Press",
     "Move handles after closing the spline"},
    {int(CloseMethod::OnClick),
     "ON_CLICK",
     0,
     "On Click",
     "Spline closes on release if not dragged"},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class PenModal : int8_t {
  FreeAlignToggle = 0,
  MoveAdjacent = 1,
  MoveEntire = 2,
  LinkHandles = 3,
  LockAngle = 4,
};

/* Used to scale the default select distance. */
constexpr float selection_distance_factor = 0.9f;

struct PenToolOperation {
  ViewContext vc;

  GreasePencil *grease_pencil;

  float threshold_distance;

  bool extrude_point;
  bool delete_point;
  bool insert_point;
  bool move_seg;
  bool select_point;
  bool move_point;
  bool toggle_vector;
  bool close_spline;
  CloseMethod close_spline_method;
  int extrude_handle;

  float4x4 projection;
};

static void grease_pencil_pen_update_view(bContext *C, PenToolOperation &ptd)
{
  GreasePencil *grease_pencil = ptd.grease_pencil;

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, grease_pencil);

  ED_region_tag_redraw(ptd.vc.region);
}

static float2 pen_global_to_screen(const PenToolOperation &ptd, const float3 &point)
{
  return ED_view3d_project_float_v2_m4(ptd.vc.region, point, ptd.projection);
}

static float3 pen_screen_to_global(const PenToolOperation &ptd,
                                   const float2 screen_co,
                                   const float3 depth_point)
{
  float3 proj_point;
  ED_view3d_win_to_3d(ptd.vc.v3d, ptd.vc.region, depth_point, screen_co, proj_point);
  return proj_point;
}

/* Will return -1 if no points are near. */
static int pen_find_closest_point(const PenToolOperation &ptd,
                                  const bke::CurvesGeometry &curves,
                                  const float2 mouse_co)
{
  float closest_distance_squared = std::numeric_limits<float>::max();
  int closest_point = -1;

  const Span<float3> positions = curves.positions();

  for (const int i : curves.points_range()) {
    const float2 pos_proj = pen_global_to_screen(ptd, positions[i]);
    const float distance_squared = math::distance_squared(pos_proj, mouse_co);

    /* Save the closest point. */
    if (distance_squared < closest_distance_squared &&
        distance_squared < ptd.threshold_distance * ptd.threshold_distance)
    {
      closest_point = i;
      closest_distance_squared = distance_squared;
    }
  }

  return closest_point;
}

static bke::CurvesGeometry pen_extrude_curves(const bke::CurvesGeometry &src)
{
  const OffsetIndices<int> points_by_curve = src.points_by_curve();

  const int old_curves_num = src.curves_num();
  const int old_points_num = src.points_num();

  const IndexMask &points_to_extrude = src.points_range().take_back(1);

  Vector<int> dst_to_src_points(old_points_num);
  array_utils::fill_index_range(dst_to_src_points.as_mutable_span());

  Vector<int> dst_to_src_curves(old_curves_num);
  array_utils::fill_index_range(dst_to_src_curves.as_mutable_span());

  Vector<bool> dst_selected(old_points_num, false);

  Vector<int> dst_curve_counts(old_curves_num);
  offset_indices::copy_group_sizes(
      points_by_curve, src.curves_range(), dst_curve_counts.as_mutable_span());

  const VArray<bool> &src_cyclic = src.cyclic();

  /* Point offset keeps track of the points inserted. */
  int point_offset = 0;
  for (const int curve_index : src.curves_range()) {
    const IndexRange curve_points = points_by_curve[curve_index];
    const IndexMask curve_points_to_extrude = points_to_extrude.slice_content(curve_points);
    const bool curve_cyclic = src_cyclic[curve_index];

    curve_points_to_extrude.foreach_index([&](const int src_point_index) {
      if (!curve_cyclic && (src_point_index == curve_points.first())) {
        /* Start-point extruded, we insert a new point at the beginning of the curve.
         * NOTE: all points of a cyclic curve behave like an inner-point. */
        dst_to_src_points.insert(src_point_index + point_offset, src_point_index);
        dst_selected.insert(src_point_index + point_offset, true);
        ++dst_curve_counts[curve_index];
        ++point_offset;
        return;
      }
      if (!curve_cyclic && (src_point_index == curve_points.last())) {
        /* End-point extruded, we insert a new point at the end of the curve.
         * NOTE: all points of a cyclic curve behave like an inner-point. */
        dst_to_src_points.insert(src_point_index + point_offset + 1, src_point_index);
        dst_selected.insert(src_point_index + point_offset + 1, true);
        ++dst_curve_counts[curve_index];
        ++point_offset;
        return;
      }

      /* Inner-point extruded: we create a new curve made of two points located at the same
       * position. Only one of them is selected so that the other one remains stuck to the curve.
       */
      dst_to_src_points.append(src_point_index);
      dst_selected.append(false);
      dst_to_src_points.append(src_point_index);
      dst_selected.append(true);
      dst_to_src_curves.append(curve_index);
      dst_curve_counts.append(2);
    });
  }

  const int new_points_num = dst_to_src_points.size();
  const int new_curves_num = dst_to_src_curves.size();

  bke::CurvesGeometry dst(new_points_num, new_curves_num);
  BKE_defgroup_copy_list(&dst.vertex_group_names, &src.vertex_group_names);

  /* Setup curve offsets, based on the number of points in each curve. */
  MutableSpan<int> new_curve_offsets = dst.offsets_for_write();
  array_utils::copy(dst_curve_counts.as_span(), new_curve_offsets.drop_back(1));
  offset_indices::accumulate_counts_to_offsets(new_curve_offsets);

  /* Attributes. */
  const bke::AttributeAccessor src_attributes = src.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst.attributes_for_write();

  /* Selection attribute. */
  /* Copy the value of control point selections to all selection attributes.
   *
   * This will lead to the extruded control point always having both handles selected, if it's a
   * bezier type stroke. This is to circumvent the issue of source curves handles not being
   * deselected when the user extrudes a bezier control point with both handles selected. */
  for (const StringRef selection_attribute_name :
       ed::curves::get_curves_selection_attribute_names(src))
  {
    bke::GSpanAttributeWriter selection = ed::curves::ensure_selection_attribute(
        dst, bke::AttrDomain::Point, bke::AttrType::Bool, selection_attribute_name);
    selection.span.copy_from(dst_selected.as_span());
    selection.finish();
  }

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         {},
                         dst_to_src_curves,
                         dst_attributes);

  /* Cyclic attribute : newly created curves cannot be cyclic. */
  dst.cyclic_for_write().drop_front(old_curves_num).fill(false);

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Point,
                         bke::AttrDomain::Point,
                         bke::attribute_filter_from_skip_ref(
                             {".selection", ".selection_handle_left", ".selection_handle_right"}),
                         dst_to_src_points,
                         dst_attributes);

  dst.update_curve_types();
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
  const float2 mouse_co = float2(event->mval);
  ptd.threshold_distance = ED_view3d_select_dist_px() * selection_distance_factor;

  ptd.extrude_point = RNA_boolean_get(op->ptr, "extrude_point");
  ptd.delete_point = RNA_boolean_get(op->ptr, "delete_point");
  ptd.insert_point = RNA_boolean_get(op->ptr, "insert_point");
  ptd.move_seg = RNA_boolean_get(op->ptr, "move_segment");
  ptd.select_point = RNA_boolean_get(op->ptr, "select_point");
  ptd.move_point = RNA_boolean_get(op->ptr, "move_point");
  ptd.toggle_vector = RNA_boolean_get(op->ptr, "toggle_vector");
  ptd.close_spline = RNA_boolean_get(op->ptr, "close_spline");
  ptd.close_spline_method = CloseMethod(RNA_enum_get(op->ptr, "close_spline_method"));
  ptd.extrude_handle = RNA_enum_get(op->ptr, "extrude_handle");

  const Scene *scene = ptd.vc.scene;
  Object *object = ptd.vc.obact;

  /* Add a modal handler for this operator. */
  WM_event_add_modal_handler(C, op);

  if (!(ELEM(event->type, LEFTMOUSE) && ELEM(event->val, KM_PRESS, KM_DBL_CLICK))) {
    return OPERATOR_RUNNING_MODAL;
  }

  std::atomic<bool> add_single = true;

  std::atomic<bool> changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene,
                                                                         *ptd.grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    const int closest_point = pen_find_closest_point(ptd, curves, mouse_co);

    if (closest_point == -1) {
      if (ptd.extrude_point) {
        IndexMaskMemory memory;
        const IndexMask selection = retrieve_editable_and_selected_points(
            *object, info.drawing, info.layer_index, memory);

        if (!selection.is_empty()) {
          add_single.store(false, std::memory_order_relaxed);

          const float3 depth_point = curves.is_empty() ? float3(0.0f) : curves.positions().last();
          curves = pen_extrude_curves(curves);

          curves.positions_for_write().last() = pen_screen_to_global(ptd, mouse_co, depth_point);
        }

        info.drawing.tag_topology_changed();

        changed.store(true, std::memory_order_relaxed);
      }
      return;
    }

    const OffsetIndices points_by_curve = curves.points_by_curve();
    const Array<int> point_to_curve_map = curves.point_to_curve_map();

    const int curve_index = point_to_curve_map[closest_point];
    const IndexRange points = points_by_curve[curve_index];

    bke::SpanAttributeWriter<bool> selection =
        curves.attributes_for_write().lookup_or_add_for_write_span<bool>(
            ".selection",
            bke::AttrDomain::Point,
            bke::AttributeInitVArray(VArray<bool>::from_single(true, curves.points_num())));

    if (ptd.close_spline) {
      if (closest_point == points.first() && selection.span[points.last()]) {
        curves.cyclic_for_write()[curve_index] = true;
        info.drawing.tag_topology_changed();
      }
      if (closest_point == points.last() && selection.span[points.first()]) {
        curves.cyclic_for_write()[curve_index] = true;
        info.drawing.tag_topology_changed();
      }
    }

    if (event->val != KM_DBL_CLICK && !ptd.delete_point) {
      selection.span.fill(false);
    }

    if (ptd.select_point) {
      selection.span[closest_point] = true;
    }

    selection.finish();

    if (ptd.delete_point) {
      curves.remove_points(IndexRange::from_single(closest_point), {});
    }

    changed.store(true, std::memory_order_relaxed);
  });

  if (add_single) {
    BLI_assert(ptd.grease_pencil->has_active_layer());
    bke::greasepencil::Drawing *drawing = ptd.grease_pencil->get_editable_drawing_at(
        *ptd.grease_pencil->get_active_layer(), vc.scene->r.cfra);

    bke::CurvesGeometry &curves = drawing->strokes_for_write();

    const float3 depth_point = curves.is_empty() ? float3(0.0f) : curves.positions().last();

    ed::greasepencil::add_single_curve(curves, true);
    bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

    /* Initialize the rest of the attributes with default values. */
    bke::fill_attribute_range_default(attributes,
                                      bke::AttrDomain::Curve,
                                      bke::attribute_filter_from_skip_ref({"position"}),
                                      curves.curves_range().take_front(1));

    curves.update_curve_types();
    curves.positions_for_write().last() = pen_screen_to_global(ptd, mouse_co, depth_point);
  }

  if (changed) {
    grease_pencil_pen_update_view(C, ptd);
  }

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

/* Modal handler: Events handling during interactive part. */
static wmOperatorStatus grease_pencil_pen_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  PenToolOperation &ptd = *reinterpret_cast<PenToolOperation *>(op->customdata);

  const Scene *scene = ptd.vc.scene;
  Object *object = ptd.vc.obact;
  GreasePencil &grease_pencil = *ptd.grease_pencil;

  if (ISMOUSE_MOTION(event->type)) {
  }
  else {
    grease_pencil_pen_exit(C, op);
    return OPERATOR_FINISHED;
  }

  std::atomic<bool> changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask selection = retrieve_editable_and_selected_points(
        *object, info.drawing, info.layer_index, memory);
    if (selection.is_empty()) {
      return;
    }

    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    MutableSpan<float3> positions = curves.positions_for_write();

    for (const int i : positions.index_range()) {
      positions[i] += float3(1.0f, 1.0f, 1.0f) * 0.001f;
    }

    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  });

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
  RNA_def_enum(ot->srna,
               "close_spline_method",
               prop_close_method,
               int(CloseMethod::Off),
               "Close Spline Method",
               "The condition for close spline to activate");
  RNA_def_boolean(
      ot->srna, "toggle_vector", false, "Toggle Vector", "Toggle between Vector and Auto handles");
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
