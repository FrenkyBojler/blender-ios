/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_geo_closure.hh"
#include "NOD_geometry_nodes_closure_eval.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_search_link.hh"

#include "BLO_read_write.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_test_closure_eagerly_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry");
  b.add_input<decl::Closure>("Closure");

  b.add_output<decl::Geometry>("Geometry");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  static const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(SOCK_GEOMETRY);

  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  ClosurePtr closure = params.extract_input<ClosurePtr>("Closure");

  if (closure) {
    ClosureEagerEvalParams eager_params;
    eager_params.inputs.append({SocketInterfaceKey("Geometry"), stype, &geometry_set});
    eager_params.outputs.append({SocketInterfaceKey("Geometry"), stype, &geometry_set});
    eager_params.user_data = params.user_data();
    evaluate_closure_eagerly(*closure, eager_params);
  }

  params.set_output("Geometry", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeTestClosureEagerly");
  ntype.ui_name = "Test Closure Eagerly";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_test_closure_eagerly_cc
