/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_self_object_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Object>("Self Object").context_identifier(".self_object");
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSelfObject", GEO_NODE_SELF_OBJECT);
  ntype.ui_name = "Self Object";
  ntype.ui_description =
      "Retrieve the object that contains the geometry nodes modifier currently being executed";
  ntype.enum_name_legacy = "SELF_OBJECT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.has_context_outputs = true;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_self_object_cc
