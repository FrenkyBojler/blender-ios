/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include <btBulletDynamicsCommon.h>

namespace blender::nodes::node_geo_bullet_physics_solver_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bundle>("Data");
  b.add_output<decl::Bundle>("Data").align_with_previous();
  b.add_input<decl::Bundle>("Behavior");
  b.add_input<decl::Float>("Delta Time").min(0).hide_value();
  b.add_input<decl::Int>("Substeps").default_value(0).min(0);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  auto collision_configuration = std::make_unique<btDefaultCollisionConfiguration>();
  auto collision_dispatcher = std::make_unique<btCollisionDispatcher>(
      collision_configuration.get());
  auto broadphase = std::make_unique<btDbvtBroadphase>();
  auto solver = std::make_unique<btSequentialImpulseConstraintSolver>();
  auto dynamics_world = std::make_unique<btDiscreteDynamicsWorld>(
      collision_dispatcher.get(), broadphase.get(), solver.get(), collision_configuration.get());

  dynamics_world->setGravity(btVector3(0.0f, -9.8f, 0.0f));

  params.set_default_remaining_outputs();
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeBulletPhysicsSolver");
  ntype.ui_name = "Bullet Physics Solver";
  ntype.ui_description = "Simulate physics using the Bullet physics engine";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_bullet_physics_solver_cc
