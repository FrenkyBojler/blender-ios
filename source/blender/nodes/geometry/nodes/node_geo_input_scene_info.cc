/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_scene_types.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_scene_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("Start Frame");
  b.add_output<decl::Int>("End Frame");
  b.add_output<decl::Float>("Frame Rate");

  b.add_output<decl::Int>("Resolution X");
  b.add_output<decl::Int>("Resolution Y");
  b.add_output<decl::Vector>("Aspect");

  b.add_output<decl::Vector>("Gravity");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const Scene *scene = DEG_get_evaluated_scene(params.depsgraph());
  if (!scene) {
    params.set_default_remaining_outputs();
    return;
  }
  const double frame_rate = double(scene->r.frs_sec) / double(scene->r.frs_sec_base);
  float res_scale = scene->r.size * 0.01f;

  params.set_output("Start Frame", scene->r.sfra);
  params.set_output("End Frame", scene->r.efra);
  params.set_output("Frame Rate", float(frame_rate));

  params.set_output("Resolution X", int(scene->r.xsch * res_scale));
  params.set_output("Resolution Y", int(scene->r.ysch * res_scale));
  params.set_output("Aspect", float3{scene->r.xasp, scene->r.yasp, 0.0f});

  params.set_output("Gravity", float3(scene->physics_settings.gravity));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeInputSceneInfo");
  ntype.ui_name = "Scene Info";
  ntype.ui_description = "Retrieve scene information";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_scene_info_cc
