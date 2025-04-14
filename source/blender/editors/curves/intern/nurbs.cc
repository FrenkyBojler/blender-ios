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
  const Span<float> weights;
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
      weights(curves.nurbs_weights().slice(curve_points)),
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
  return ikcd.knot_range.x + (ikcd.knot_range.y - ikcd.knot_range.x) * normalized;
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

  op->customdata = &ikcd;
  op->flag |= OP_IS_MODAL_CURSOR_REGION;
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus insert_knot_apply(InsertKnotOpData &ikcd)
{
  if (ikcd.preview_positions.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  const int new_points_added = ikcd.preview_positions.size() - ikcd.points_to_replace.size();
  CurvesGeometry new_curves = bke::curves::copy_only_curve_domain(ikcd.curves);
  new_curves.resize(ikcd.curves.points_num() + new_points_added, ikcd.curves.curves_num());
  new_curves.nurbs_knots_modes_for_write()[ikcd.curve] = NURBS_KNOT_MODE_CUSTOM;
  MutableSpan<int> offsets = new_curves.offsets_for_write();
  offsets.copy_from(ikcd.curves.offsets());
  for (const int i : new_curves.curves_range().drop_front(ikcd.curve)) {
    offsets[i + 1] += new_points_added;
  }

  new_curves.nurbs_custom_knots_update_size();
  const IndexRange curve_points = ikcd.curve_points;
  const IndexRange new_curve_points = IndexRange::from_begin_size(
      curve_points.first(), curve_points.size() + new_points_added);
  const IndexRange curve_knots = new_curves.nurbs_custom_knots_by_curve()[ikcd.curve];
  const Span<float> knots = ikcd.knots;
  const bool cyclic = ikcd.curves.cyclic()[ikcd.curve];
  MutableSpan<float> new_knots = new_curves.nurbs_custom_knots_for_write().slice(curve_knots);

  const int shifted_span = ikcd.knot_span % curve_points.size();
  const float shifted_knot = ikcd.knot_to_insert - knots[ikcd.knot_span] + knots[shifted_span];

  new_knots.take_front(shifted_span + 1).copy_from(knots.take_front(shifted_span + 1));
  const IndexRange inserted_knots = IndexRange::from_begin_size(shifted_span + 1,
                                                                new_points_added);
  new_knots.slice(inserted_knots).fill(shifted_knot);
  MutableSpan<float> unchanged_knots_tail = new_knots.drop_front(inserted_knots.one_after_last());
  unchanged_knots_tail.copy_from(knots.slice(inserted_knots.first(), unchanged_knots_tail.size()));
  if (cyclic) {
    MutableSpan<float> cyclic_tail = new_knots.drop_front(new_curve_points.size());
    for (const int i : cyclic_tail.index_range()) {
      cyclic_tail[i] = cyclic_tail.first() + new_knots[i] - new_knots.first();
    }
  }

  const bke::AttributeAccessor src_attributes = ikcd.curves.attributes();
  bke::MutableAttributeAccessor dst_attributes = new_curves.attributes_for_write();

  const int dst_shift = (ikcd.points_to_replace.last() + new_points_added) <= curve_points.last() ?
                            0 :
                            new_points_added;
  const IndexRange points_before = IndexRange::from_begin_end(
      0, curve_points.first() + (dst_shift ? 0 : ikcd.points_to_replace.first()));
  const IndexRange points_after = IndexRange::from_begin_end(
      curve_points.first() + (dst_shift ? 0 : ikcd.points_to_replace.last()),
      ikcd.curves.points_num());

  Array<bool> is_altered(new_curve_points.size());
  const IndexRange altered_points_range = IndexRange::from_begin_size(
      ikcd.points_to_replace.first() - ikcd.curve_points.first() + dst_shift,
      ikcd.points_to_replace.size() + new_points_added);
  for (const int i : altered_points_range) {
    is_altered[i % new_curve_points.size()] = true;
  }
  IndexMaskMemory memory;
  const IndexMask altered_points = IndexMask::from_bools(
      IndexRange(new_curve_points.size()), is_altered, memory);
  const int8_t order = ikcd.order;
  const IndexRange src_point_range = IndexRange::from_begin_size(
      ikcd.points_to_replace.one_before_start(), order);
  const Span<float> control_weights = ikcd.curves.nurbs_weights().slice(curve_points);

  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           ATTR_DOMAIN_MASK_POINT,
           bke::attribute_filter_from_skip_ref(
               ed::curves::get_curves_selection_attribute_names(ikcd.curves))))
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
          Span<float> point_weights = ikcd.point_weights.as_span().slice(j * order, order);
          for (const int i : point_weights.index_range()) {
            const int src_i = (ikcd.knot_span - ikcd.order + 1 + i) % curve_points.size();
            mixer.mix_in(
                altered_point, src_points[src_i], control_weights[src_i] * point_weights[i]);
          }
        };
        mixer.finalize(altered_points);
      }
    });
    attribute.dst.finish();
  }

  ikcd.curves = new_curves;
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
        ED_region_tag_redraw(ikcd.region);
        ikcd.curves.tag_topology_changed();

        DEG_id_tag_update(&(ikcd.curves_id->id), ID_RECALC_GEOMETRY);
        WM_event_add_notifier(C, NC_GEOM | ND_DATA, ikcd.curves_id);
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

      bke::curves::nurbs::find_span_mult(
          ikcd.knot_to_insert, ikcd.knots, ikcd.order, ikcd.knot_span, knot_multiplicity);
      if (knot_multiplicity + repeat <= ikcd.order - 1) {
        ikcd.points_to_replace = bke::curves::nurbs::calc_knot_insertion_weights(
            ikcd.knots,
            ikcd.order,
            ikcd.knot_to_insert,
            ikcd.knot_span,
            knot_multiplicity,
            repeat,
            ikcd.point_weights);
        ikcd.preview_positions = ikcd.preview_positions_buffer.as_mutable_span().slice(
            0, ikcd.points_to_replace.size() + repeat);

        MutableSpan<float3> preview_positions = ikcd.preview_positions;
        for (const int altered_point : preview_positions.index_range()) {
          float4 position = float4(0.0f);
          for (const int i : IndexRange(ikcd.order)) {
            const float point_weight = ikcd.point_weights[altered_point * ikcd.order + i];
            const int src = (ikcd.knot_span - ikcd.order + 1 + i) % ikcd.curve_points.size();
            const float4 src_position = float4(ikcd.positions[src] * ikcd.weights[src],
                                               ikcd.weights[src]);
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
  ot->modal = nurbs::insert_knot_modal;
  ot->cancel = nurbs::insert_knot_exit;
  ot->poll = editable_curves_in_edit_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

}  // namespace blender::ed::curves
