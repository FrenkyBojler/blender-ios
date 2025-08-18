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

/* Used to scale the default select distance. */
constexpr float selection_distance_factor = 0.9f;
constexpr float selection_distance_factor_edge = 0.5f;

/* Total number of curve handle types. */
constexpr int CURVE_HANDLE_TYPES_NUM = 4;

using namespace blender::ed::curves::pen_tool;

class GreasePencilPenToolOperation : public PenToolOperation {
 public:
  GreasePencil *grease_pencil;
  Vector<MutableDrawingInfo> drawings;

  /* Helper class to project screen space coordinates to 3D. */
  DrawingPlacement placement;

  float3 project(const float2 &screen_co) const
  {
    return this->placement.project(screen_co);
  }
};

static void grease_pencil_pen_update_view(bContext *C, GreasePencilPenToolOperation &ptd)
{
  GreasePencil *grease_pencil = ptd.grease_pencil;

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, grease_pencil);

  ED_region_tag_redraw(ptd.vc.region);
}

static ClosestElement pen_find_closest_element(const GreasePencilPenToolOperation &ptd,
                                               const float2 &mouse_co)
{
  ClosestElement closest_element;
  closest_element.element_mode = ElementMode::None;

  for (const int drawing_index : ptd.drawings.index_range()) {
    const MutableDrawingInfo &info = ptd.drawings[drawing_index];
    const bke::CurvesGeometry &curves = info.drawing.strokes();
    const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
    const float4x4 layer_to_object = layer.local_transform();

    IndexMaskMemory memory;
    const IndexMask editable_points = ed::greasepencil::retrieve_editable_points(
        *ptd.vc.obact, info.drawing, info.layer_index, memory);
    const IndexMask editable_curves = ed::greasepencil::retrieve_editable_strokes(
        *ptd.vc.obact, info.drawing, info.layer_index, memory);
    const IndexMask bezier_points = ed::greasepencil::retrieve_visible_bezier_handle_points(
        *ptd.vc.obact, info.drawing, info.layer_index, ptd.vc.v3d->overlay.handle_display, memory);

    pen_find_closest_point(
        ptd, curves, editable_points, layer_to_object, drawing_index, mouse_co, closest_element);
    pen_find_closest_handle(
        ptd, curves, bezier_points, layer_to_object, drawing_index, mouse_co, closest_element);
    pen_find_closest_edge_point(
        ptd, curves, editable_curves, layer_to_object, drawing_index, mouse_co, closest_element);
  }
  return closest_element;
}

/**
 * Will return true if a new curve can be created, and report any errors.
 */
static bool pen_can_create_new_curve(const GreasePencilPenToolOperation &ptd, wmOperator *op)
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

static float2 calculate_center_of_mass(const GreasePencilPenToolOperation &ptd,
                                       const bool ends_only)
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

        if (point_i != points.first() && point_i != points.last()) {
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

/* Invoke handler: Initialize the operator. */
static wmOperatorStatus grease_pencil_pen_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* If in tools region, wait till we get to the main (3D-space)
   * region before allowing drawing to take place. */
  op->flag |= OP_IS_MODAL_CURSOR_REGION;

  ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_depsgraph_pointer(C));

  if (vc.scene->toolsettings->gpencil_selectmode_edit != GP_SELECTMODE_POINT) {
    BKE_report(op->reports, RPT_ERROR, "Selection Mode must be Points");
    return OPERATOR_CANCELLED;
  }

  wmWindow *win = CTX_wm_window(C);
  /* Set cursor to indicate modal. */
  WM_cursor_modal_set(win, WM_CURSOR_CROSS);

  /* Allocate new data. */
  GreasePencilPenToolOperation *ptd_pointer = MEM_new<GreasePencilPenToolOperation>(__func__);
  op->customdata = ptd_pointer;
  GreasePencilPenToolOperation &ptd = *ptd_pointer;

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

  threading::parallel_for(ptd.drawings.index_range(), 1, [&](const IndexRange drawing_range) {
    for (const int drawing_index : drawing_range) {
      const MutableDrawingInfo &info = ptd.drawings[drawing_index];
      bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

      if (curves.is_empty()) {
        continue;
      }

      if (ptd.closest_element.element_mode == ElementMode::Edge) {
        add_single.store(false, std::memory_order_relaxed);
        if (ptd.insert_point) {
          ptd.insert_point_to_curve(curves);
          info.drawing.tag_topology_changed();
          changed.store(true, std::memory_order_relaxed);
        }
        continue;
      }

      if (ptd.closest_element.element_mode == ElementMode::None) {
        if (ptd.extrude_point) {
          const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
          const float4x4 layer_to_object = layer.local_transform();

          IndexMaskMemory memory;
          const IndexMask editable_curves = ed::greasepencil::retrieve_editable_strokes(
              *ptd.vc.obact, info.drawing, info.layer_index, memory);
          const bke::CurvesGeometry &src = info.drawing.strokes();

          if (std::optional<bke::CurvesGeometry> result = ptd.extrude_curves(
                  src, layer_to_object, editable_curves))
          {
            curves = std::move(*result);
          }
          else {
            for (const StringRef selection_attribute_name :
                 ed::curves::get_curves_selection_attribute_names(curves))
            {
              bke::GSpanAttributeWriter selection_writer = ed::curves::ensure_selection_attribute(
                  curves, bke::AttrDomain::Point, bke::AttrType::Bool, selection_attribute_name);
              ed::curves::fill_selection_false(selection_writer.span);
              selection_writer.finish();
            }
            continue;
          }

          add_single.store(false, std::memory_order_relaxed);
          point_added.store(true, std::memory_order_relaxed);
          info.drawing.tag_topology_changed();

          changed.store(true, std::memory_order_relaxed);
          continue;
        }

        continue;
      }

      if (drawing_index != ptd.closest_element.drawing_index) {
        if (event->val != KM_DBL_CLICK && !ptd.delete_point) {
          for (const StringRef selection_attribute_name :
               ed::curves::get_curves_selection_attribute_names(curves))
          {
            bke::GSpanAttributeWriter selection_writer = ed::curves::ensure_selection_attribute(
                curves, bke::AttrDomain::Point, bke::AttrType::Bool, selection_attribute_name);
            ed::curves::fill_selection_false(selection_writer.span);
            selection_writer.finish();
          }
        }

        continue;
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
        continue;
      }

      const bool clear_selection = event->val != KM_DBL_CLICK && !ptd.delete_point;
      if (ptd.close_curve_and_select(curves, points, clear_selection)) {
        info.drawing.tag_topology_changed();
        add_single.store(false, std::memory_order_relaxed);
      }

      changed.store(true, std::memory_order_relaxed);
    }
  });

  if (add_single) {
    if (pen_can_create_new_curve(ptd, op)) {
      bke::greasepencil::Layer &layer = *ptd.grease_pencil->get_active_layer();
      bke::greasepencil::Drawing *drawing = ptd.grease_pencil->get_editable_drawing_at(
          layer, ptd.vc.scene->r.cfra);
      bke::CurvesGeometry &curves = drawing->strokes_for_write();
      const float4x4 layer_to_world = layer.to_world_space(*ptd.vc.obact);

      ptd.add_single_point_and_curve(curves, layer_to_world);
      drawing->opacities_for_write().last() = 1.0f;
      drawing->tag_topology_changed();

      changed.store(true, std::memory_order_relaxed);
      point_added = true;
    }
  }

  pen_status_indicators(C, op);
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
  GreasePencilPenToolOperation *ptd = static_cast<GreasePencilPenToolOperation *>(op->customdata);

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
  GreasePencilPenToolOperation &ptd = *reinterpret_cast<GreasePencilPenToolOperation *>(
      op->customdata);

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
    else if (event->val == int(PenModal::MoveHandle)) {
      ptd.move_handle = !ptd.move_handle;
    }
  }

  std::atomic<bool> changed = false;
  ptd.center_of_mass_co = calculate_center_of_mass(ptd, false);

  if (ptd.move_seg && ptd.closest_element.element_mode == ElementMode::Edge) {
    const MutableDrawingInfo &info = ptd.drawings[ptd.closest_element.drawing_index];
    const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
    const float4x4 layer_to_world = layer.to_world_space(*ptd.vc.obact);
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    ptd.move_segment(curves, layer_to_world);
    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  }
  else {
    threading::parallel_for_each(ptd.drawings, [&](const MutableDrawingInfo &info) {
      bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
      const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
      const float4x4 layer_to_object = layer.local_transform();
      const float4x4 layer_to_world = layer.to_world_space(*ptd.vc.obact);

      IndexMaskMemory memory;
      const IndexMask bezier_points = ed::greasepencil::retrieve_visible_bezier_handle_points(
          *ptd.vc.obact,
          info.drawing,
          info.layer_index,
          ptd.vc.v3d->overlay.handle_display,
          memory);

      if (ptd.move_handles_in_curve(curves, bezier_points, layer_to_world, layer_to_object)) {
        changed.store(true, std::memory_order_relaxed);
        info.drawing.tag_topology_changed();
      }
    });
  }

  pen_status_indicators(C, op);
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

  /* Properties. */
  ed::curves::pen_tool::pen_tool_common_props(ot);
}

}  // namespace blender::ed::greasepencil

void ED_operatortypes_grease_pencil_pen()
{
  using namespace blender::ed::greasepencil;
  WM_operatortype_append(GREASE_PENCIL_OT_pen);
}

void ED_grease_pencil_pentool_modal_keymap(wmKeyConfig *keyconf)
{
  using namespace blender::ed::curves::pen_tool;
  static const EnumPropertyItem modal_items[] = {
      {int(PenModal::MoveHandle),
       "MOVE_HANDLE",
       0,
       "Move Current Handle",
       "Move the current handle of the control point freely"},
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
