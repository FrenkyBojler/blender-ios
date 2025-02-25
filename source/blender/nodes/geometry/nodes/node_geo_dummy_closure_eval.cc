/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"

#include "BLI_math_vector_types.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_dummy_closure_eval_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Closure>("Closure");
  b.add_input<decl::Float>("Value");
  b.add_output<decl::Geometry>("Geometry");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_default_remaining_outputs();
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeDummyClosureEval");
  ntype.ui_name = "Dummy Closure Eval";
  ntype.ui_description = "";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_dummy_closure_eval_cc
