/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_offset_indices.hh"

#include "BKE_curves.hh"
#include "BKE_lib_id.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_geometry_nodes_values.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_usage_inference.hh"

#include "list_function_eval.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curves_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Geometry>("Curves"_ustr);

  b.add_input<decl::Int>("Points"_ustr)
      .default_value(1)
      .min(1)
      .description("The total number of points");
  b.add_input<decl::Vector>("Positions"_ustr).structure_type(StructureType::Field).hide_value();
  b.add_input<decl::Int>("Curves"_ustr)
      .default_value(1)
      .min(1)
      .description("The number of curves");
  b.add_input<decl::Int>("Curve Sizes"_ustr)
      .default_value(1)
      .min(1)
      .hide_value()
      .structure_type(StructureType::Field)
      .description("The number of points in each curve");
}

static Curves *create_curves_from_topology_info(GeoNodeExecParams &params,
                                                const int points_num,
                                                GField &positions_field,
                                                const int curves_num,
                                                GField &curve_sizes_field)
{
  Curves *curves_id = bke::curves_new_nomain(points_num, curves_num);
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  ListFieldContext context;
  fn::FieldEvaluator evaluator_curve_sizes{context, curves_num};
  evaluator_curve_sizes.add_with_destination(std::move(curve_sizes_field),
                                             curves.offsets_for_write());
  evaluator_curve_sizes.evaluate();

  if (std::any_of(curves.offsets().begin(),
                  curves.offsets().drop_back(1).end(),
                  [](const int size) { return size < 1; }))
  {
    params.error_message_add(NodeWarningType::Error, "Curve sizes must be at least 1");
    BKE_id_free_ex(nullptr, curves_id, LIB_ID_FREE_NO_MAIN, false);
    return nullptr;
  }

  auto curve_offsets = offset_indices::accumulate_counts_to_offsets(curves.offsets_for_write());
  if (curve_offsets.total_size() != points_num) {
    params.error_message_add(NodeWarningType::Error,
                             "Curve sizes must sum to the number of points");
    BKE_id_free_ex(nullptr, curves_id, LIB_ID_FREE_NO_MAIN, false);
    return nullptr;
  }

  fn::FieldEvaluator evaluator_positions{context, points_num};
  evaluator_positions.add_with_destination(std::move(positions_field),
                                           curves.positions_for_write());
  evaluator_positions.evaluate();

  curves.fill_curve_types(CURVE_TYPE_POLY);

  return curves_id;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const int points_num = params.extract_input<int>("Points"_ustr);
  const int curves_num = params.extract_input<int>("Curves"_ustr);
  if (points_num < 1) {
    params.error_message_add(NodeWarningType::Error, "Number of points must be greater than zero");
    params.set_default_remaining_outputs();
    return;
  }
  if (curves_num < 1) {
    params.error_message_add(NodeWarningType::Error, "Number of curves must be greater than zero");
    params.set_default_remaining_outputs();
    return;
  }
  if (points_num < curves_num) {
    params.error_message_add(
        NodeWarningType::Error,
        "Number of curves must be less than or equal to the number of points");
    params.set_default_remaining_outputs();
    return;
  }
  if (params.output_is_required("Curves"_ustr)) {
    GField positions_field = params.extract_input<GField>("Positions"_ustr);
    GField curve_sizes_field = params.extract_input<GField>("Curve Sizes"_ustr);

    Curves *curves = create_curves_from_topology_info(
        params, points_num, positions_field, curves_num, curve_sizes_field);
    params.set_output("Curves"_ustr, GeometrySet::from_curves(curves));
  }
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeCurves"_ustr);
  ntype.ui_name = "Curves";
  ntype.ui_description = "Create new curves from positions and curve offsets";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curves_cc
