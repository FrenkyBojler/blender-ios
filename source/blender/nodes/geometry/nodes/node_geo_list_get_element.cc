/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_list_get_element_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  const bNode *node = b.node_or_null();

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_output(type, "Value");
  }

  b.add_input<decl::Int>("Index").min(0);

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "List").structure_type(StructureType::List);
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const int index = params.extract_input<int>("Index");
  if (index <= 0) {
    params.set_default_remaining_outputs();
    return;
  }

  const ListPtr list = params.extract_input<ListPtr>("List");
  if (!list) {
    params.set_default_remaining_outputs();
    return;
  }

  if (index >= list->values().size()) {
    params.set_default_remaining_outputs();
    return;
  }

  const GSpan values = list->values();
  const GPointer value = values[index];
  params.set_output("Value", value);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeListGetElement");
  ntype.ui_name = "Get Element";
  ntype.ui_description = "Retrieve an element from a list";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_get_element_cc
