/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"

#include "BLI_array_utils.hh"

#include "NOD_geometry_nodes_list.hh"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_nurbs_knots_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Curves")
      .supported_type({GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil})
      .description("NURBS Curve to change the knot sequence of");
  b.add_output<decl::Geometry>("Curves").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input(SOCK_FLOAT, "Knot").structure_type(StructureType::List).hide_value();
}

/**
 * Used to check the knot sequence of each curve without executing `count_nonzero_knot_spans()`
 * every time. The array tells how many breakpoints exist ahead of a specified knot.
 * If greater than zero at the degree-th knot (or first knot for cyclic curve), the curve knot
 * sequence is valid.
 */
static Array<int> reversed_accumulation_delta(const VArray<float> &array)
{
  const int delta_size = array.size() - 1;
  Array<int> result(delta_size);
  int accumulation = 0;
  for (const int i : result.index_range()) {
    if (array[delta_size - i] - array[delta_size - 1 - i] > 0.0f) {
      accumulation++;
    }
    result[delta_size - 1 - i] = accumulation;
  }
  return result;
}

static void set_curves_knots(bke::CurvesGeometry &curves,
                             const fn::FieldContext &field_context,
                             const Field<bool> &selection_field,
                             const VArray<float> &new_knot_sequence,
                             std::atomic<bool> &has_selected_curve,
                             std::atomic<bool> &has_nurbs,
                             std::atomic<bool> &any_affected,
                             const Array<int> &knot_validator)
{
  if (!curves.has_curve_with_type(CURVE_TYPE_NURBS)) {
    return;
  }
  has_nurbs = true;

  fn::FieldEvaluator selection_evaluator{field_context, curves.curves_num()};
  selection_evaluator.add(selection_field);
  selection_evaluator.evaluate();
  const IndexMask selection = selection_evaluator.get_evaluated_as_mask(0);
  if (selection.is_empty()) {
    return;
  }
  has_selected_curve = true;

  /* Sets knot mode for every matching curve. */
  const VArray<int8_t> curve_types = curves.curve_types();
  const OffsetIndices points_by_curve = curves.points_by_curve();
  const VArray<int8_t> nurbs_orders = curves.nurbs_orders();
  const VArray<bool> cyclic = curves.cyclic();
  IndexMaskMemory memory;

  const IndexMask curves_to_write = IndexMask::from_predicate(
      selection,
      memory,
      [&](const int i_curve) {
        if (curve_types[i_curve] != CURVE_TYPE_NURBS) {
          return false;
        }
        const int points_num = points_by_curve[i_curve].size();
        const int order = nurbs_orders[i_curve];
        const bool is_cyclic = cyclic[i_curve];
        return points_num + order == new_knot_sequence.size() &&
               (knot_validator[order - 1] > 0 || (knot_validator.first() > 0 && is_cyclic));
      },
      exec_mode::grain_size(2048));

  if (curves_to_write.is_empty()) {
    return;
  }

  const Array<int> custom_knots_by_curve_src(curves.nurbs_custom_knots_by_curve().data());
  const Array<float> custom_knots_src(curves.nurbs_custom_knots());

  index_mask::masked_fill(
      curves.nurbs_knots_modes_for_write(), int8_t(NURBS_KNOT_MODE_CUSTOM), curves_to_write);
  any_affected = true;
  curves.nurbs_custom_knots_update_size();

  MutableSpan<float> custom_knots = curves.nurbs_custom_knots_for_write();
  const OffsetIndices custom_knots_by_curve_dst = curves.nurbs_custom_knots_by_curve();
  const IndexMask curves_to_preserve = IndexMask::from_difference(
      IndexMask(curves.curves_num()), curves_to_write, memory);

  /* Writes back existing knots. */
  array_utils::copy_group_to_group(OffsetIndices<int>(custom_knots_by_curve_src),
                                   custom_knots_by_curve_dst,
                                   curves_to_preserve,
                                   custom_knots_src.as_span(),
                                   custom_knots);

  /* Writes new knots. */
  curves_to_write.foreach_index(
      [&](const int i_curve) {
        const IndexRange dst = custom_knots_by_curve_dst[i_curve];
        new_knot_sequence.materialize(custom_knots.slice(dst));
      },
      exec_mode::grain_size(1024));
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const ListPtr input_knot = params.extract_input<ListPtr>("Knot");

  std::atomic<bool> has_selected_curve = false;
  std::atomic<bool> has_nurbs = false;
  std::atomic<bool> any_affected = false;

  if (!input_knot) {
    params.set_output("Curves", std::move(geometry_set));
    return;
  }

  const VArray<float> input_knot_span = input_knot->varray<float>();

  /* Used to check knot sequence without using `count_nonzero_knot_spans()` for each curve.*/
  const Array<int> knot_validator = reversed_accumulation_delta(input_knot_span);

  if (knot_validator.first() == 0) {
    params.error_message_add(NodeWarningType::Error, TIP_("Invalid knot sequence"));
    params.set_output("Curves", std::move(geometry_set));
    return;
  }

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (Curves *curves_id = geometry_set.get_curves_for_write()) {
      bke::CurvesGeometry &curves = curves_id->geometry.wrap();
      const bke::CurvesFieldContext field_context{*curves_id, AttrDomain::Curve};
      set_curves_knots(curves,
                       field_context,
                       selection_field,
                       input_knot_span,
                       has_selected_curve,
                       has_nurbs,
                       any_affected,
                       knot_validator);
    }
    if (GreasePencil *grease_pencil = geometry_set.get_grease_pencil_for_write()) {
      using namespace blender::bke::greasepencil;
      for (const int layer_index : grease_pencil->layers().index_range()) {
        Drawing *drawing = grease_pencil->get_eval_drawing(grease_pencil->layer(layer_index));
        if (drawing == nullptr) {
          continue;
        }
        bke::CurvesGeometry &curves = drawing->strokes_for_write();
        const bke::GreasePencilLayerFieldContext field_context{
            *grease_pencil, AttrDomain::Curve, layer_index};
        set_curves_knots(curves,
                         field_context,
                         selection_field,
                         input_knot_span,
                         has_selected_curve,
                         has_nurbs,
                         any_affected,
                         knot_validator);
      }
    }
  });

  if (has_selected_curve) {
    if (!has_nurbs) {
      params.error_message_add(NodeWarningType::Info, TIP_("Input curves do not have NURBS type"));
    }
    else if (!any_affected) {
      params.error_message_add(NodeWarningType::Info,
                               TIP_("Input knot size does not match any NURBS curve"));
    }
  }

  params.set_output("Curves", std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetNURBSKnots");
  ntype.ui_name = "Set NURBS Knots";
  ntype.ui_description =
      "Control the spreading of NURBS curves points by assigning a \"knot vector\"";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_nurbs_knots_cc
