/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_physics_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_physics_time_step_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Delta Time").default_value(0.0167f);
  b.add_input<decl::Int>("Collision Steps").default_value(1);
  b.add_input<decl::Geometry>("Physics").supported_type(GeometryComponent::Type::Physics);
  b.add_output<decl::Geometry>("Physics").propagate_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const float delta_time = params.extract_input<float>("Delta Time");
  const int collision_steps = params.extract_input<int>("Collision Steps");
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Physics");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (!geometry_set.has_physics()) {
      return;
    }

    if (bke::PhysicsGeometry *physics = geometry_set.get_physics_for_write()) {
      physics->state_for_write().step_simulation(delta_time, collision_steps);
    }
  });

  params.set_output("Physics", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "PhysicsTimeStep", GEO_NODE_PHYSICS_TIME_STEP);
  ntype.ui_name = "Physics Time Step";
  ntype.ui_description = "Advance rigid bodies over a time interval and resolve collisions";
  ntype.enum_name_legacy = "PHYSICS_TIME_STEP";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_physics_time_step_cc
