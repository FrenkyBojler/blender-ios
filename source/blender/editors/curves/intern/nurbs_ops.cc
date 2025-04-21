#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BLI_array_utils.hh"
#include "DEG_depsgraph.hh"
#include "DNA_modifier_types.h"
#include "ED_curves.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_view3d.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"
#include "UI_resources.hh"
#include "WM_api.hh"

namespace blender::ed::curves {

using bke::CurvesGeometry;

namespace nurbs {

struct InsertKnotOpData {
  ARegion *region;   /* Region that tool was activated in. */
  void *draw_handle; /* For drawing preview loop. */

  ViewContext vc;

  Curves *curves_id;
  CurvesGeometry &curves;
  const int curve;

  const int8_t order;
  const IndexRange curve_points;
  const Span<float3> positions;
  Span<float> knots;
  Array<float> knots_buffer;

  float knot_to_insert;
  int knot_span;

  IndexRange points_to_replace;
  Array<float3> preview_positions_buffer;
  MutableSpan<float3> preview_positions;

  Array<float> point_weights;

  float2 knot_range;

  InsertKnotOpData(ARegion *region, ViewContext vc, Curves *curves_id, const int curve);
  InsertKnotOpData(const InsertKnotOpData &data);
};

InsertKnotOpData::InsertKnotOpData(ARegion *region,
                                   ViewContext vc,
                                   Curves *curves_id,
                                   const int curve)
    : region(region),
      vc(vc),
      curves_id(curves_id),
      curves(curves_id->geometry.wrap()),
      curve(curve),
      order(curves.nurbs_orders()[curve]),
      curve_points(curves.points_by_curve()[curve]),
      positions(curves.positions().slice(curve_points)),
      points_to_replace(0),
      preview_positions_buffer(2 * (order - 1) - 1),
      point_weights(preview_positions_buffer.size() * order)
{
  const bool cyclic = curves.cyclic()[curve];
  const int knots_num = bke::curves::nurbs::knots_num(curve_points.size(), order, cyclic);

  const KnotsMode knots_mode = KnotsMode(curves.nurbs_knots_modes()[curve]);
  knots_buffer.reinitialize(knots_num);
  if (knots_mode == NURBS_KNOT_MODE_CUSTOM) {
    const OffsetIndices custom_knots_by_curve = curves.nurbs_custom_knots_by_curve();
    const Span<float> custom_knots = curves.nurbs_custom_knots();
    bke::curves::nurbs::copy_custom_knots(
        order, cyclic, custom_knots.slice(custom_knots_by_curve[curve]), knots_buffer);
  }
  else {
    bke::curves::nurbs::calculate_knots(
        curve_points.size(), knots_mode, order, cyclic, knots_buffer.as_mutable_span());
  }
  knots = knots_buffer.as_span();
  knot_range = float2(knots[order - 1], knots.last(order - 1));
}

InsertKnotOpData::InsertKnotOpData(const InsertKnotOpData &data)
    : InsertKnotOpData(data.region, data.vc, data.curves_id, data.curve)
{
}

static float event_to_knot(const InsertKnotOpData &ikcd, const wmEvent &event)
{
  const int winx = ikcd.region->winx;
  const float padded_width = winx * 0.8f;
  const float normalized = std::max(
      0.0f, std::min(1.0f, (float(event.mval[0]) - winx * 0.1f) / padded_width));
  const float knot_value = ikcd.knot_range.x +
                           (ikcd.knot_range.y - ikcd.knot_range.x) * normalized;
  /* In cyclic case insertion of knot_range.x or knot_range.y are mathematically equivalent.
   * They are knots[order - 1] and knots[curve_points.size + order - 1] respectively.
   * knot_range.y messes up apply function so knot_range.x is inserted. */
  const float ret_val = knot_value == ikcd.knots[ikcd.curve_points.size() + ikcd.order - 1] ?
                            ikcd.knot_range.x :
                            knot_value;
  return ret_val;
}

static void modified_lattice_draw(const bContext * /*C*/, ARegion * /*region*/, void *arg)
{
  InsertKnotOpData &ikcd = *static_cast<InsertKnotOpData *>(arg);
  if (ikcd.preview_positions.is_empty()) {
    return;
  }

  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_blend(GPU_BLEND_ALPHA);

  GPU_matrix_push();
  GPU_matrix_mul(ikcd.vc.obedit->object_to_world().ptr());

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  Span<float3> preview_positions = ikcd.preview_positions;
  if (preview_positions.size() > 1) {
    float viewport[4];
    GPU_viewport_size_get_f(viewport);

    immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR);
    immUniform2fv("viewportSize", &viewport[2]);
    immUniformThemeColor3(TH_GIZMO_PRIMARY);
    immUniform1f("lineWidth", U.pixelsize);
    immBegin(GPU_PRIM_LINES, (preview_positions.size() - 1) * 2);
    for (const int i : preview_positions.index_range().drop_back(1)) {
      immVertex3fv(pos, preview_positions[i]);
      immVertex3fv(pos, preview_positions[i + 1]);
    }

    immEnd();
    immUnbindProgram();
  }
  {
    GPU_program_point_size(true);
    immBindBuiltinProgram(GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA);
    immUniformThemeColor3(TH_GIZMO_SECONDARY);
    immUniform1f("size", 5 * UI_SCALE_FAC);
    immBegin(GPU_PRIM_POINTS, preview_positions.size());
    for (const float3 position : preview_positions) {
      immVertex3fv(pos, position);
    }
    immEnd();
    immUnbindProgram();
  }
  GPU_matrix_pop();

  /* Reset default */
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPU_blend(GPU_BLEND_NONE);
}

static wmOperatorStatus insert_knot_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *object = CTX_data_edit_object(C);
  Curves *active_curves_id = static_cast<Curves *>(object->data);

  CurvesGeometry &curves = active_curves_id->geometry.wrap();
  IndexMaskMemory memory;
  const IndexMask selected_curves = retrieve_selected_curves(curves, memory);

  op->customdata = nullptr;

  bool is_nurbs_selected = false;
  int curve = -1;
  if (selected_curves.size() == 1) {
    curve = selected_curves.first();
    is_nurbs_selected = curves.curve_types()[curve] == CURVE_TYPE_NURBS;
  }

  if (!is_nurbs_selected) {
    BKE_report(op->reports, RPT_ERROR, "One NURBS curve has to be selected");
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  InsertKnotOpData &ikcd = *MEM_new<InsertKnotOpData>(__func__,
                                                      CTX_wm_region(C),
                                                      ED_view3d_viewcontext_init(C, depsgraph),
                                                      active_curves_id,
                                                      curve);
  ikcd.draw_handle = ED_region_draw_cb_activate(
      ikcd.region->runtime->type, modified_lattice_draw, &ikcd, REGION_DRAW_POST_VIEW);

  ikcd.knot_to_insert = RNA_float_get(op->ptr, "knot");

  op->customdata = &ikcd;
  op->flag |= OP_IS_MODAL_CURSOR_REGION;
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus insert_knot_exec(bContext *C, wmOperator *op)
{
  RNA_float_set(op->ptr, "knot", 0.5);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus insert_knot_apply(InsertKnotOpData &ikcd)
{
  if (ikcd.preview_positions.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  ikcd.curves = ed::curves::nurbs::insert_knot(ikcd.curves, ikcd.curve, ikcd.knot_to_insert, 1);
  return OPERATOR_FINISHED;
}

static void insert_knot_exit(bContext * /*C*/, wmOperator *op)
{
  InsertKnotOpData &ikcd = *static_cast<InsertKnotOpData *>(op->customdata);

  /* deactivate the extra drawing stuff in 3D-View */
  ED_region_draw_cb_exit(ikcd.region->runtime->type, ikcd.draw_handle);

  ED_region_tag_redraw(ikcd.region);

  MEM_delete(&ikcd);
  op->customdata = nullptr;
}

static wmOperatorStatus insert_knot_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  InsertKnotOpData &ikcd = *static_cast<InsertKnotOpData *>(op->customdata);
  switch (event->type) {
    case EVT_RETKEY:
    case EVT_PADENTER:
    case LEFTMOUSE: /* confirm */
      if (event->val == KM_PRESS) {
        wmOperatorStatus retVal = insert_knot_apply(ikcd);
        ikcd.curves.tag_topology_changed();

        DEG_id_tag_update(&(ikcd.curves_id->id), ID_RECALC_GEOMETRY);
        WM_event_add_notifier(C, NC_GEOM, ikcd.curves_id);
        RNA_float_set(op->ptr, "knot", 0.5);
        insert_knot_exit(C, op);
        return retVal;
      }
      break;
    case RIGHTMOUSE:
      insert_knot_exit(C, op);
      return OPERATOR_CANCELLED;
    case MOUSEMOVE: {
      const int repeat = 1;
      int knot_multiplicity;
      ikcd.knot_to_insert = event_to_knot(ikcd, *event);

      ed::curves::nurbs::find_span_mult(
          ikcd.knot_to_insert, ikcd.knots, ikcd.order, ikcd.knot_span, knot_multiplicity);
      if (knot_multiplicity + repeat <= ikcd.order - 1) {
        ikcd.points_to_replace = ed::curves::nurbs::calc_knot_insertion_weights(
            ikcd.knots,
            ikcd.order,
            ikcd.knot_to_insert,
            ikcd.knot_span,
            knot_multiplicity,
            repeat,
            ikcd.point_weights);
        ikcd.preview_positions = ikcd.preview_positions_buffer.as_mutable_span().slice(
            0, ikcd.points_to_replace.size() + repeat);

        const Array<float> weights = make_weights_for_knot_span(
            ikcd.order, ikcd.curves.nurbs_weights(), ikcd.curve_points, ikcd.knot_span);

        MutableSpan<float3> preview_positions = ikcd.preview_positions;
        for (const int altered_point : preview_positions.index_range()) {
          float4 position = float4(0.0f);
          for (const int i : IndexRange(ikcd.order)) {
            const float point_weight = ikcd.point_weights[altered_point * ikcd.order + i];
            const int src = (ikcd.knot_span - ikcd.order + 1 + i) % ikcd.curve_points.size();
            const float4 src_position = float4(ikcd.positions[src] * weights[i], weights[i]);
            position += src_position * point_weight;
          }
          preview_positions[altered_point] = position.xyz() / position.w;
        }
      }
      else {
        ikcd.points_to_replace = IndexRange(0);
        ikcd.preview_positions = {};
      }
      break;
    }
    default: {
      break;
    }
  }
  ED_region_tag_redraw(ikcd.region);
  /* keep going until the user confirms */
  return OPERATOR_RUNNING_MODAL;
}

}  // namespace nurbs

void CURVES_OT_nurbs_insert_knot(wmOperatorType *ot)
{
  ot->name = "Insert NURBS knot";
  ot->idname = __func__;
  ot->description = "Select NURBS curve and insert new knot";

  ot->invoke = nurbs::insert_knot_invoke;
  ot->exec = nurbs::insert_knot_exec;
  ot->modal = nurbs::insert_knot_modal;
  ot->cancel = nurbs::insert_knot_exit;
  ot->poll = editable_curves_in_edit_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;
  prop = RNA_def_float(
      ot->srna, "knot", 0.0f, -100.0f, 100.0f, "Knot", "Knot value to insert", -100.0f, 100.0f);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

}  // namespace blender::ed::curves
