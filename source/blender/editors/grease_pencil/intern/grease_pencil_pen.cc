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

/* Total number of curve handle types. */
constexpr int CURVE_HANDLE_TYPES_NUM = 4;

using namespace blender::ed::curves::pen_tool;

static IndexMask retrieve_editable_and_all_selected_points(
    Object &object,
    const bke::greasepencil::Drawing &drawing,
    int layer_index,
    IndexMaskMemory &memory)
{
  const bke::CurvesGeometry &curves = drawing.strokes();

  const IndexMask editable_points = retrieve_editable_points(object, drawing, layer_index, memory);
  const IndexMask selected_points = ed::curves::retrieve_all_selected_points(curves, memory);

  return IndexMask::from_intersection(editable_points, selected_points, memory);
}

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

  IndexMask all_selected_points(const int curves_index, IndexMaskMemory &memory) const
  {
    const MutableDrawingInfo &info = this->drawings[curves_index];
    return ed::greasepencil::retrieve_editable_and_all_selected_points(
        *this->vc.obact, info.drawing, info.layer_index, memory);
  }

  IndexMask editable_curves(const int curves_index, IndexMaskMemory &memory) const
  {
    const MutableDrawingInfo &info = this->drawings[curves_index];
    return ed::greasepencil::retrieve_editable_strokes(
        *this->vc.obact, info.drawing, info.layer_index, memory);
  }

  void tag_curve_changed(const int curves_index) const
  {
    const MutableDrawingInfo &info = this->drawings[curves_index];
    info.drawing.tag_topology_changed();
  }

  bke::CurvesGeometry &get_curves(const int curves_index) const
  {
    const MutableDrawingInfo &info = this->drawings[curves_index];
    return info.drawing.strokes_for_write();
  }

  IndexRange curves_range() const
  {
    return this->drawings.index_range();
  }

  void single_point_attributes(bke::CurvesGeometry & /*curves*/, const int curves_index) const
  {
    const MutableDrawingInfo &info = this->drawings[curves_index];
    info.drawing.opacities_for_write().last() = 1.0f;
  }

  bool can_create_new_curve(wmOperator *op) const
  {
    if (!this->grease_pencil->has_active_layer()) {
      BKE_report(op->reports, RPT_ERROR, "No active Grease Pencil layer");
      return false;
    }

    bke::greasepencil::Layer &layer = *this->grease_pencil->get_active_layer();
    if (!layer.is_editable()) {
      BKE_report(op->reports, RPT_ERROR, "Active layer is locked or hidden");
      return false;
    }

    const int material_index = this->vc.obact->actcol - 1;
    Material *material = BKE_object_material_get(this->vc.obact, material_index + 1);
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
            *this->vc.scene, *this->grease_pencil, layer, false, inserted_keyframe))
    {
      BKE_report(op->reports, RPT_ERROR, "No Grease Pencil frame to draw on");
      return false;
    }

    BLI_assert(this->active_drawing_index != std::nullopt);

    return true;
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
    const float4x4 &layer_to_object = ptd.layer_to_objects[drawing_index];

    IndexMaskMemory memory;
    const IndexMask editable_points = ed::greasepencil::retrieve_editable_points(
        *ptd.vc.obact, info.drawing, info.layer_index, memory);
    const IndexMask bezier_points = ed::greasepencil::retrieve_visible_bezier_handle_points(
        *ptd.vc.obact, info.drawing, info.layer_index, ptd.vc.v3d->overlay.handle_display, memory);
    const IndexMask editable_curves = ptd.editable_curves(drawing_index, memory);

    pen_find_closest_point(
        ptd, curves, editable_points, layer_to_object, drawing_index, mouse_co, closest_element);
    pen_find_closest_handle(
        ptd, curves, bezier_points, layer_to_object, drawing_index, mouse_co, closest_element);
    pen_find_closest_edge_point(
        ptd, curves, editable_curves, layer_to_object, drawing_index, mouse_co, closest_element);
  }
  return closest_element;
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

  /* Allocate new data. */
  GreasePencilPenToolOperation *ptd_pointer = MEM_new<GreasePencilPenToolOperation>(__func__);
  op->customdata = ptd_pointer;
  GreasePencilPenToolOperation &ptd = *ptd_pointer;

  if (ptd.initialize(C, op, event)) {
    return OPERATOR_RUNNING_MODAL;
  }

  if (ptd.vc.scene->toolsettings->gpencil_selectmode_edit != GP_SELECTMODE_POINT) {
    BKE_report(op->reports, RPT_ERROR, "Selection Mode must be Points");
    return OPERATOR_CANCELLED;
  }

  GreasePencil *grease_pencil = static_cast<GreasePencil *>(ptd.vc.obact->data);
  ptd.grease_pencil = grease_pencil;
  View3D *view3d = CTX_wm_view3d(C);

  /* Initialize helper class for projecting screen space coordinates. */
  DrawingPlacement placement = DrawingPlacement(
      *ptd.vc.scene, *ptd.vc.region, *view3d, *ptd.vc.obact, grease_pencil->get_active_layer());
  if (placement.use_project_to_surface()) {
    placement.cache_viewport_depths(CTX_data_depsgraph_pointer(C), ptd.vc.region, view3d);
  }
  else if (placement.use_project_to_stroke()) {
    placement.cache_viewport_depths(CTX_data_depsgraph_pointer(C), ptd.vc.region, view3d);
  }

  ptd.placement = placement;

  std::atomic<bool> add_single = ptd.extrude_point;
  std::atomic<bool> changed = false;
  std::atomic<bool> point_added = false;
  std::atomic<bool> point_removed = false;
  ptd.drawings = retrieve_editable_drawings(*ptd.vc.scene, *ptd.grease_pencil);

  for (const int drawing_index : ptd.drawings.index_range()) {
    const MutableDrawingInfo &info = ptd.drawings[drawing_index];
    const bke::greasepencil::Layer &layer = ptd.grease_pencil->layer(info.layer_index);
    ptd.layer_to_objects.append(layer.local_transform());
    ptd.layer_to_worlds.append(layer.to_world_space(*ptd.vc.obact));
  }

  ptd.active_drawing_index = std::nullopt;
  const bke::greasepencil::Layer *active_layer = ptd.grease_pencil->get_active_layer();

  if (active_layer != nullptr) {
    const bke::greasepencil::Drawing *active_drawing = ptd.grease_pencil->get_editable_drawing_at(
        *active_layer, ptd.vc.scene->r.cfra);

    for (const int drawing_index : ptd.drawings.index_range()) {
      const MutableDrawingInfo &info = ptd.drawings[drawing_index];

      if (active_drawing == &info.drawing) {
        BLI_assert(ptd.active_drawing_index == std::nullopt);
        ptd.active_drawing_index = drawing_index;
      }
    }
  }

  ptd.center_of_mass_co = ptd.calculate_center_of_mass(true);
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
          const float4x4 &layer_to_object = ptd.layer_to_objects[drawing_index];

          IndexMaskMemory memory;
          const IndexMask editable_curves = ptd.editable_curves(drawing_index, memory);
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
    if (ptd.can_create_new_curve(op)) {
      const int curves_index = *ptd.active_drawing_index;

      const float4x4 &layer_to_world = ptd.layer_to_worlds[curves_index];
      bke::CurvesGeometry &curves = ptd.get_curves(curves_index);

      ptd.add_single_point_and_curve(curves, layer_to_world);
      ptd.single_point_attributes(curves, curves_index);
      ptd.tag_curve_changed(curves_index);

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

  if (std::optional<wmOperatorStatus> result = modal_start(ptd, C, op, event)) {
    if (*result == OPERATOR_FINISHED) {
      grease_pencil_pen_exit(C, op);
    }
    return *result;
  }

  std::atomic<bool> changed = false;
  ptd.center_of_mass_co = ptd.calculate_center_of_mass(false);

  if (ptd.move_seg && ptd.closest_element.element_mode == ElementMode::Edge) {
    const MutableDrawingInfo &info = ptd.drawings[ptd.closest_element.drawing_index];
    const float4x4 &layer_to_world = ptd.layer_to_worlds[ptd.closest_element.drawing_index];
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    ptd.move_segment(curves, layer_to_world);
    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  }
  else {
    threading::parallel_for(ptd.drawings.index_range(), 1, [&](const IndexRange drawing_range) {
      for (const int drawing_index : drawing_range) {
        const MutableDrawingInfo &info = ptd.drawings[drawing_index];
        bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
        const float4x4 &layer_to_object = ptd.layer_to_objects[drawing_index];
        const float4x4 &layer_to_world = ptd.layer_to_worlds[drawing_index];

        IndexMaskMemory memory;
        const IndexMask selection = ptd.all_selected_points(drawing_index, memory);

        if (ptd.move_handles_in_curve(curves, selection, layer_to_world, layer_to_object)) {
          changed.store(true, std::memory_order_relaxed);
          ptd.tag_curve_changed(drawing_index);
        }
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

  wmKeyMap *keymap = ensure_keymap(keyconf);
  WM_modalkeymap_assign(keymap, "GREASE_PENCIL_OT_pen");
}
