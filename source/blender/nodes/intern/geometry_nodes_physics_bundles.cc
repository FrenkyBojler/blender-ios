/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_bundle_type.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"
#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

namespace blender::nodes {

template<typename T>
inline void parse_member(const Bundle &bundle,
                         const StringRef name,
                         T &r_value,
                         BehaviorParseErrors &r_errors)
{
  if (const std::optional<T> value = bundle.lookup<T>(name)) {
    r_value = *value;
  }
  else {
    r_errors.wrong_members.append(name);
  }
}

const FlatBundleTypePtr &GravityBehavior::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(GravityBehavior::name);
    b.add<decl::Vector>("gravity").default_value(float3(0.0f, 0.0f, -9.81f));
    FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<GravityBehavior> GravityBehavior::parse(const Bundle &bundle,
                                                      BehaviorParseErrors &r_errors)
{
  GravityBehavior behavior;
  parse_member(bundle, "gravity", behavior.gravity, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &ForceBehavior::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(ForceBehavior::name);
    b.add<decl::String>("filter");
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Vector>("force").supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<ForceBehavior> ForceBehavior::parse(const Bundle &bundle,
                                                  BehaviorParseErrors &r_errors)
{
  ForceBehavior behavior;
  parse_member(bundle, "filter", behavior.filter, r_errors);
  parse_member(bundle, "selection", behavior.selection, r_errors);
  parse_member(bundle, "force", behavior.force, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &RigidBodyInstancesBehavior::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(RigidBodyInstancesBehavior::name);
    b.add<decl::Geometry>("instances").supported_type(bke::GeometryComponent::Type::Instance);
    b.add<decl::Int>("collision_shape_type").supports_field();
    b.add<decl::Int>("motion_type").supports_field();
    b.add<decl::Float>("friction").default_value(0.5f).supports_field();
    b.add<decl::Float>("bounciness").default_value(0.0f).min(0.0f).supports_field();
    b.add<decl::Float>("density").default_value(1000.0f).min(0.0f).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<RigidBodyInstancesBehavior> RigidBodyInstancesBehavior::parse(
    const Bundle &bundle, BehaviorParseErrors &r_errors)
{
  RigidBodyInstancesBehavior behavior;
  parse_member(bundle, "instances", behavior.instances_geometry, r_errors);
  parse_member(bundle, "collision_shape_type", behavior.collision_shape_type, r_errors);
  parse_member(bundle, "motion_type", behavior.motion_type, r_errors);
  parse_member(bundle, "friction", behavior.friction, r_errors);
  parse_member(bundle, "bounciness", behavior.bounciness, r_errors);
  parse_member(bundle, "density", behavior.density, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  behavior.instances_geometry.keep_only({bke::GeometryComponent::Type::Instance});
  return behavior;
}

const FlatBundleTypePtr &SoftBodyMeshBehavior::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(SoftBodyMeshBehavior::name);
    b.add<decl::Geometry>("mesh").supported_type(bke::GeometryComponent::Type::Mesh);
    b.add<decl::Float>("stretch_stiffness").default_value(1e6f).min(0.0f).supports_field();
    b.add<decl::Float>("bend_stiffness").default_value(1e6f).min(0.0f).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<SoftBodyMeshBehavior> SoftBodyMeshBehavior::parse(const Bundle &bundle,
                                                                BehaviorParseErrors &r_errors)
{
  SoftBodyMeshBehavior behavior;
  parse_member(bundle, "mesh", behavior.mesh_geometry, r_errors);
  parse_member(bundle, "stretch_stiffness", behavior.stretch_stiffness, r_errors);
  parse_member(bundle, "bend_stiffness", behavior.bend_stiffness, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  behavior.mesh_geometry.keep_only({bke::GeometryComponent::Type::Mesh});
  return behavior;
}

}  // namespace blender::nodes
