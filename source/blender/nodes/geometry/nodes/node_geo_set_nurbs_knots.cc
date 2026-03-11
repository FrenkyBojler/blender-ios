/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"

#include "NOD_geometry_nodes_list.hh"

#include "GEO_foreach_geometry.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_nurbs_knots_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves")
      .supported_type({GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil})
      .description("Curve to convert to a mesh using the given profile");
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input(SOCK_FLOAT, "Knot").structure_type(StructureType::List).hide_value();
  b.add_output<decl::Geometry>("Curves").propagate_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curves");
  ListPtr input_knot = params.extract_input<ListPtr>("Knot");

  std::atomic<bool> has_curves = false;
  std::atomic<bool> has_nurbs = false;
  std::atomic<bool> any_affected = false;

  // TODO : grease pencil
  // TODO : selection

  if (input_knot) {
    geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
      if (Curves *curves_id = geometry_set.get_curves_for_write()) {
        bke::CurvesGeometry &curves = curves_id->geometry.wrap();

        if (curves.has_curve_with_type(CURVE_TYPE_NURBS)) {
          const VArray<int8_t> curve_types = curves.curve_types();
          const OffsetIndices points_by_curve = curves.points_by_curve();
          const VArray<int8_t> nurbs_orders = curves.nurbs_orders();
          const VArray<bool> cyclic = curves.cyclic();
          MutableSpan<int8_t> knot_mode = curves.nurbs_knots_modes_for_write();

          // Make space for every NURBS with the right knot length
          for (const int i_curve : curves.curves_range()) {
            if (curve_types[i_curve] != CURVE_TYPE_NURBS) {
              continue;
            }
            const int points_num = points_by_curve[i_curve].size();
            const int order = nurbs_orders[i_curve];
            const bool is_cyclic = cyclic[i_curve];
            const int knot_num_i_curve = bke::curves::nurbs::knots_num(
                points_num, order, is_cyclic);

            if (knot_num_i_curve == input_knot->size()) {
              knot_mode[i_curve] = NURBS_KNOT_MODE_CUSTOM;
              any_affected = true;
            }
          }
          curves.nurbs_custom_knots_update_size();

          // Write the new knots
          const OffsetIndices custom_knots_by_curve = curves.nurbs_custom_knots_by_curve();
          MutableSpan<float> custom_knots = curves.nurbs_custom_knots_for_write();

          IndexMaskMemory memory;
          const IndexMask custom_knot_curves = curves.nurbs_custom_knot_curves(memory);
          custom_knot_curves.foreach_index(
              [&](const int i_curve) {
                const IndexRange dst = custom_knots_by_curve[i_curve];
                if (dst.size() == input_knot->size()) {
                  int i = 0;
                  input_knot->foreach<float>([&](const float knot_value) {
                    custom_knots[dst.start() + i] = knot_value;
                    i++;
                  });
                }
              },
              exec_mode::grain_size(512));

          curves.nurbs_custom_knots_update_size();
        }
      }
    });
  }
  if (has_curves) {
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
      "Controls the spreading of NURBS curve points by assigning it a \"knot vector\"";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_nurbs_knots_cc
