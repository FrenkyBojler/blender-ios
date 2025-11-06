/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GEO_lsystem.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_lsystem_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Axiom");
  b.add_output<decl::Geometry>("Geometry");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const std::string axiom = params.extract_input<std::string>("Axiom");
  geometry::lsystem::LSystemParams lsystem_params;
  lsystem_params.axiom = axiom;

  std::variant<bke::CurvesGeometry, std::string> result = geometry::lsystem::lsystem_to_curves(
      lsystem_params);
  if (const std::string *error = std::get_if<std::string>(&result)) {
    params.error_message_add(NodeWarningType::Error, *error);
    params.set_default_remaining_outputs();
    return;
  }
  bke::CurvesGeometry &curves_geometry = std::get<bke::CurvesGeometry>(result);
  Curves *curves_id = bke::curves_new_nomain(std::move(curves_geometry));
  params.set_output("Geometry", GeometrySet::from_curves(curves_id));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeLSystem");
  ntype.ui_name = "L-System";
  ntype.ui_description = "Create geometry using a L-System";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_lsystem_cc
