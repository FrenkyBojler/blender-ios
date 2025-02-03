/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_tool_3d_cursor_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Location")
      .subtype(PROP_TRANSLATION)
      .description(
          "The location of the scene's 3D cursor, in the local space of the modified object")
      .context_identifier(".3d_cursor_location");
  b.add_output<decl::Rotation>("Rotation")
      .description(
          "The rotation of the scene's 3D cursor, in the local space of the modified object")
      .context_identifier(".3d_cursor_rotation");
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeTool3DCursor", GEO_NODE_TOOL_3D_CURSOR);
  ntype.ui_name = "3D Cursor";
  ntype.ui_description = "The scene's 3D cursor location and rotation";
  ntype.enum_name_legacy = "TOOL_3D_CURSOR";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = search_link_ops_for_tool_node;
  ntype.has_context_outputs = true;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_tool_3d_cursor_cc
