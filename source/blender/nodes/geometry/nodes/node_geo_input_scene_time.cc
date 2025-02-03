/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_scene_time_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Seconds").context_identifier(".scene_time_seconds");
  b.add_output<decl::Float>("Frame").context_identifier(".scene_time_frame");
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeInputSceneTime", GEO_NODE_INPUT_SCENE_TIME);
  ntype.ui_name = "Scene Time";
  ntype.ui_description =
      "Retrieve the current time in the scene's animation in units of seconds or frames";
  ntype.enum_name_legacy = "INPUT_SCENE_TIME";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.has_context_outputs = true;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_scene_time_cc
