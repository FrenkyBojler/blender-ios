/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_implicit_sharing.hh"

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
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Geometry>("Curves"_ustr);

  b.add_input<decl::Int>("Points"_ustr)
      .default_value(1)
      .min(1)
      .description("The number of points in the curves");
  b.add_input<decl::Vector>("Positions"_ustr).structure_type(StructureType::Field).hide_value();
  b.add_input<decl::Int>("Curve Offsets"_ustr).structure_type(StructureType::List).hide_value();
}

static bool get_offsets_from_list(GeoNodeExecParams &params,
                                   bke::CurvesGeometry &curves,
                                   const GListPtr &offsets_list)
{
  if (!offsets_list->cpp_type().is<int>()) {
    params.error_message_add(NodeWarningType::Error, "Curve Offsets must be a list of integers");
    return false;
  }
  const GList::DataVariant &offsets_list_data = offsets_list->data();
  const auto *array_data = std::get_if<GList::ArrayData>(&offsets_list_data);
  if (!array_data) {
    params.error_message_add(NodeWarningType::Error, "Curve Offsets can't be a single integer");
    return false;
  }
  const auto values = offsets_list->typed<int>().values();
  const auto *span_values = std::get_if<Span<int>>(&values);
  BLI_assert(span_values);

  const Span<int> offsets = *span_values;
  if (offsets.is_empty() || offsets.first() != 0) {
    params.error_message_add(NodeWarningType::Error, "The first curve offset must be zero");
    return false;
  }
  if (offsets.last() != curves.points_num()) {
    params.error_message_add(NodeWarningType::Error,
                             "The last curve offset must be equal to the number of points");
    return false;
  }
  for (const int i : offsets.index_range().drop_back(1)) {
    if (offsets[i] >= offsets[i + 1]) {
      params.error_message_add(NodeWarningType::Error,
                               "Curve offsets must be in ascending order");
      return false;
    }
  }
  const int curves_num = offsets.size() - 1;
  curves.attribute_storage.wrap().resize(AttrDomain::Curve, curves_num);
  implicit_sharing::copy_shared_pointer(static_cast<int *>(const_cast<void *>(array_data->data)), &(*array_data->sharing_info), &curves.curve_offsets, &curves.runtime->curve_offsets_sharing_info);
  curves.curve_num = curves_num;
  return true;
}

static Curves *create_curves_from_topology_info(GeoNodeExecParams &params,
                                                 const int points_num,
                                                 GField &positions_field,
                                                 const GListPtr &offsets_list)
{
  if (!offsets_list) {
    params.error_message_add(NodeWarningType::Error, "The curve offsets are required");
    return nullptr;
  }
  const int offsets_num = offsets_list->size();
  if (offsets_num < 2) {
    params.error_message_add(NodeWarningType::Error,
                             "There must be at least one curve (at least 2 offsets)");
    return nullptr;
  }

  /* The curve offsets might get shared from the list, so start with no curves. */
  Curves *curves_id = bke::curves_new_nomain(points_num, 0);
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  if (!get_offsets_from_list(params, curves, offsets_list)) {
    BKE_id_free_ex(nullptr, curves_id, LIB_ID_FREE_NO_MAIN, false);
    return nullptr;
  }

  ListFieldContext context;
  fn::FieldEvaluator evaluator{context, points_num};
  evaluator.add_with_destination(std::move(positions_field), curves.positions_for_write());
  evaluator.evaluate();

  curves.fill_curve_types(CURVE_TYPE_POLY);

  return curves_id;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const int points_num = params.extract_input<int>("Points"_ustr);
  if (points_num < 1) {
    params.error_message_add(NodeWarningType::Error,
                             "Number of points must be greater than zero");
    params.set_default_remaining_outputs();
    return;
  }
  if (params.output_is_required("Curves"_ustr)) {
    GField positions_field = params.extract_input<GField>("Positions"_ustr);
    const GListPtr curves_offset = params.extract_input<GListPtr>("Curve Offsets"_ustr);

    Curves *curves = create_curves_from_topology_info(
        params, points_num, positions_field, curves_offset);
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