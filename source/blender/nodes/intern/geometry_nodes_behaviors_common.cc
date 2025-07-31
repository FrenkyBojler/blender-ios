/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_behaviors_common.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_list.hh"
#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

namespace blender::nodes {

const std::shared_ptr<BehaviorDef> &GravityBehavior::def()
{
  static const std::shared_ptr<BehaviorDef> def = []() {
    auto def = std::make_shared<BehaviorDef>();
    def->type = GravityBehavior::type;
    def->add<decl::Vector>("gravity").default_value(float3(0.0f, 0.0f, -9.81f));
    return def;
  }();
  return def;
}

std::optional<GravityBehavior> GravityBehavior::parse(const Bundle &bundle,
                                                      BehaviorParseErrors &r_errors)
{
  GravityBehavior behavior;
  behaviors::parse_member(bundle, "gravity", behavior.gravity, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const std::shared_ptr<BehaviorDef> &ForceBehavior::def()
{
  static const std::shared_ptr<BehaviorDef> def = []() {
    auto def = std::make_shared<BehaviorDef>();
    def->type = ForceBehavior::type;
    def->add<decl::String>("filter");
    def->add<decl::Bool>("selection").default_value(true).supports_field();
    def->add<decl::Vector>("force").supports_field();
    return def;
  }();
  return def;
}

std::optional<ForceBehavior> ForceBehavior::parse(const Bundle &bundle,
                                                  BehaviorParseErrors &r_errors)
{
  ForceBehavior behavior;
  behaviors::parse_member(bundle, "filter", behavior.filter, r_errors);
  behaviors::parse_member(bundle, "selection", behavior.selection, r_errors);
  behaviors::parse_member(bundle, "force", behavior.force, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const std::shared_ptr<BehaviorDef> &RigidBodyInstancesBehavior::def()
{
  static const std::shared_ptr<BehaviorDef> def = []() {
    auto def = std::make_shared<BehaviorDef>();
    def->type = RigidBodyInstancesBehavior::type;
    def->add<decl::Geometry>("instances").supported_type(bke::GeometryComponent::Type::Instance);
    def->add<decl::Int>("collision_shape_type").supports_field();
    def->add<decl::Int>("motion_type").supports_field();
    def->add<decl::Float>("friction").default_value(0.5f).supports_field();
    def->add<decl::Float>("bounciness").default_value(0.0f).min(0.0f).supports_field();
    def->add<decl::Float>("density").default_value(1000.0f).min(0.0f).supports_field();
    return def;
  }();
  return def;
}

std::optional<RigidBodyInstancesBehavior> RigidBodyInstancesBehavior::parse(
    const Bundle &bundle, BehaviorParseErrors &r_errors)
{
  RigidBodyInstancesBehavior behavior;
  behaviors::parse_member(bundle, "instances", behavior.instances_geometry, r_errors);
  behaviors::parse_member(bundle, "collision_shape_type", behavior.collision_shape_type, r_errors);
  behaviors::parse_member(bundle, "motion_type", behavior.motion_type, r_errors);
  behaviors::parse_member(bundle, "friction", behavior.friction, r_errors);
  behaviors::parse_member(bundle, "bounciness", behavior.bounciness, r_errors);
  behaviors::parse_member(bundle, "density", behavior.density, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  behavior.instances_geometry.keep_only({bke::GeometryComponent::Type::Instance});
  return behavior;
}

const std::shared_ptr<BehaviorDef> &SoftBodyMeshBehavior::def()
{
  static const std::shared_ptr<BehaviorDef> def = []() {
    auto def = std::make_shared<BehaviorDef>();
    def->type = SoftBodyMeshBehavior::type;
    def->add<decl::Geometry>("mesh").supported_type(bke::GeometryComponent::Type::Mesh);
    def->add<decl::Float>("stretch_stiffness").default_value(1e6f).min(0.0f).supports_field();
    def->add<decl::Float>("bend_stiffness").default_value(1e6f).min(0.0f).supports_field();
    return def;
  }();
  return def;
}

std::optional<SoftBodyMeshBehavior> SoftBodyMeshBehavior::parse(const Bundle &bundle,
                                                                BehaviorParseErrors &r_errors)
{
  SoftBodyMeshBehavior behavior;
  behaviors::parse_member(bundle, "mesh", behavior.mesh_geometry, r_errors);
  behaviors::parse_member(bundle, "stretch_stiffness", behavior.stretch_stiffness, r_errors);
  behaviors::parse_member(bundle, "bend_stiffness", behavior.bend_stiffness, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  behavior.mesh_geometry.keep_only({bke::GeometryComponent::Type::Mesh});
  return behavior;
}

const std::shared_ptr<BehaviorDef> &RigidBodyConstraintDistance::def()
{
  static const std::shared_ptr<BehaviorDef> def = []() {
    auto def = std::make_shared<BehaviorDef>();
    def->type = RigidBodyConstraintDistance::type;
    def->add<decl::String>("bodies_a");
    def->add<decl::String>("bodies_b");
    def->add<decl::Int>("ids_a").structure_type(StructureType::List);
    def->add<decl::Int>("ids_b").structure_type(StructureType::List);
    return def;
  }();
  return def;
}

std::optional<RigidBodyConstraintDistance> RigidBodyConstraintDistance::parse(
    const Bundle &bundle, BehaviorParseErrors &r_errors)
{
  RigidBodyConstraintDistance behavior;
  behaviors::parse_member(bundle, "bodies_a", behavior.bodies_a, r_errors);
  behaviors::parse_member(bundle, "bodies_b", behavior.bodies_b, r_errors);
  behaviors::parse_member(bundle, "ids_a", behavior.ids_a, r_errors);
  behaviors::parse_member(bundle, "ids_b", behavior.ids_b, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  if (behavior.bodies_a.empty() || behavior.bodies_b.empty()) {
    return std::nullopt;
  }
  if (!behavior.ids_a || !behavior.ids_b) {
    return std::nullopt;
  }
  if (!behavior.ids_a->cpp_type().is<int>() || !behavior.ids_b->cpp_type().is<int>()) {
    r_errors.other_errors.append(TIP_("ID lists must be integers"));
    return std::nullopt;
  }
  if (behavior.ids_a->size() != behavior.ids_b->size()) {
    r_errors.other_errors.append(TIP_("The ID lists have different lengths"));
    return std::nullopt;
  }
  return behavior;
}

}  // namespace blender::nodes
