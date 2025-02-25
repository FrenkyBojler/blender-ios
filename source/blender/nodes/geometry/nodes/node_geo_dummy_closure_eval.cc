/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_closure_eval.hh"

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
  bke::ClosurePtr closure = params.extract_input<bke::ClosurePtr>("Closure");
  bke::SocketValueVariant value = params.extract_input<bke::SocketValueVariant>("Value");
  if (!closure) {
    params.set_default_remaining_outputs();
    return;
  }
  ResourceScope scope;
  ClosureEagerEvalParams eval_params;
  eval_params.user_data = params.user_data();
  eval_params.inputs.append(
      {bke::SocketInterfaceKey{"Value"}, bke::node_socket_type_find_static(SOCK_FLOAT), &value});
  GeometrySet output_geometry;
  std::destroy_at(&output_geometry);
  eval_params.outputs.append({bke::SocketInterfaceKey{"Geometry"},
                              bke::node_socket_type_find_static(SOCK_GEOMETRY),
                              &output_geometry});
  evaluate_closure_eagerly(*closure, eval_params);
  params.set_output("Geometry", std::move(output_geometry));
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
