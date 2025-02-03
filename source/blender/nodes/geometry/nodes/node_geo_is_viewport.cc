/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DEG_depsgraph_query.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_is_viewport_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Bool>("Is Viewport").context_identifier(".is_viewport");
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeIsViewport", GEO_NODE_IS_VIEWPORT);
  ntype.ui_name = "Is Viewport";
  ntype.ui_description =
      "Retrieve whether the nodes are being evaluated for the viewport rather than the final "
      "render";
  ntype.enum_name_legacy = "IS_VIEWPORT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.has_context_outputs = true;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_is_viewport_cc
