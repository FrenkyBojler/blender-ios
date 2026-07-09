/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <Jolt/Jolt.h>

#include "node_geometry_util.hh"

#include "GEO_jolt.hh"

namespace blender::nodes::node_geo_jolt_solver_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Bundle>("World"_ustr)
      .evaluated_geometry_field()
      .structure_type(StructureType::Single)
      .description("World state that is updated by the solver");
  b.add_output<decl::Bundle>("World"_ustr).pass_through_input_index(0).align_with_previous();

  b.add_input<decl::Float>("Delta Time"_ustr)
      .min(0)
      .default_value(1 / 25.0f)
      .subtype(PROP_TIME_ABSOLUTE);
  b.add_input<decl::Matrix>("Simulation to World"_ustr);
  {
    auto &solver_panel = b.add_panel("Solver"_ustr).default_closed(true);
    solver_panel.add_input<decl::Int>("Substeps"_ustr).default_value(1).min(1);
    solver_panel.add_input<decl::String>("Solver Path"_ustr);
  }
  {
    auto &p = b.add_panel("Interpolation Range"_ustr).default_closed(true);
    p.add_input<decl::Float>("Begin"_ustr).default_value(0.0).min(0.0);
    p.add_input<decl::Float>("End"_ustr).default_value(1.0).min(0.0);
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  geometry::jolt::ensure_initialization();
  params.set_default_remaining_outputs();
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeJoltSolver"_ustr);
  ntype.ui_name = "Jolt Solver";
  ntype.ui_description = "Physics simulation using the jolt solver";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.default_width = bke::NodeWidth::_200;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_jolt_solver_cc
