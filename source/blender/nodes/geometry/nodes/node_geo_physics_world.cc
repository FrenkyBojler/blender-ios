/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_implicit_sharing_ptr.hh"
#include "NOD_rna_define.hh"

#include "BKE_physics_geometry.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "GEO_separate_geometry.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_physics_world_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Vector>("Gravity").default_value(float3(0, 0, -9.81)).hide_value();
  b.add_output<decl::Geometry>("Geometry");
}

static void node_layout(uiLayout * /*layout*/, bContext * /*C*/, PointerRNA * /*ptr*/) {}

static void node_init(bNodeTree * /*tree*/, bNode * /*node*/) {}

static void node_geo_exec(GeoNodeExecParams params)
{
  const float3 gravity = params.extract_input<float3>("Gravity");

  bke::PhysicsGeometry *physics = new bke::PhysicsGeometry();
  physics->state_for_write().create_world();
  physics->state_for_write().set_gravity(gravity);

  params.set_output("Geometry", GeometrySet::from_physics(physics));
}

static void node_rna(StructRNA * /*srna*/) {}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "PhysicsWorld", GEO_NODE_PHYSICS_WORLD);
  ntype.ui_name = "Physics World";
  ntype.ui_description = "Generate a physics world for simulating rigid bodies";
  ntype.enum_name_legacy = "PHYSICS_WORLD";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(&ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_physics_world_cc
