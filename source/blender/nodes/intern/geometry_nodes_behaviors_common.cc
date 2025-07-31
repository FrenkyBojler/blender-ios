/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_behaviors_common.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

namespace blender::nodes {

const std::shared_ptr<BehaviorDef> &GravityBehavior::def()
{
  static const std::shared_ptr<BehaviorDef> def = []() {
    auto def = std::make_shared<BehaviorDef>();
    def->type = "COMMON_GRAVITY";
    def->add<decl::Vector>("gravity");
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
    def->type = "COMMON_FORCE";
    def->add<decl::String>("filter");
    def->add<decl::Bool>("selection").supports_field();
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
    def->type = "COMMON_RIGID_BODY_INSTANCES";
    def->add<decl::Geometry>("instances").supported_type(bke::GeometryComponent::Type::Instance);
    def->add<decl::Int>("collision_shape_type").supports_field();
    def->add<decl::Int>("motion_type").supports_field();
    def->add<decl::Float>("friction").supports_field();
    def->add<decl::Float>("bounciness").supports_field();
    def->add<decl::Float>("density").supports_field();
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
  return behavior;
}

const std::shared_ptr<BehaviorDef> &SoftBodyMeshBehavior::def()
{
  static const std::shared_ptr<BehaviorDef> def = []() {
    auto def = std::make_shared<BehaviorDef>();
    def->type = "COMMON_SOFT_BODY_MESH";
    def->add<decl::Geometry>("mesh").supported_type(bke::GeometryComponent::Type::Mesh);
    def->add<decl::Float>("stretch_stiffness").supports_field();
    def->add<decl::Float>("bend_stiffness").supports_field();
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
  return behavior;
}

}  // namespace blender::nodes
