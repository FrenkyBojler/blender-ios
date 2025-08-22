/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_type_conversions.hh"
#include "NOD_bundle_type.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"
#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

namespace blender::nodes::physics_bundles {

const FlatBundleTypePtr &GravityBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(GravityBundle::name);
    b.add<decl::Vector>("gravity").default_value(float3(0.0f, 0.0f, -9.81f));
    FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<GravityBundle> GravityBundle::parse(const Bundle &bundle,
                                                  BundleParseErrors &r_errors)
{
  GravityBundle behavior;
  bundle_parse_member(bundle, "gravity", behavior.gravity, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &ForceBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(ForceBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Vector>("force").supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<ForceBundle> ForceBundle::parse(const Bundle &bundle, BundleParseErrors &r_errors)
{
  ForceBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "selection", behavior.selection, r_errors);
  bundle_parse_member(bundle, "force", behavior.force, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &TorqueBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(TorqueBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Vector>("torque").supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<TorqueBundle> TorqueBundle::parse(const Bundle &bundle, BundleParseErrors &r_errors)
{
  TorqueBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "selection", behavior.selection, r_errors);
  bundle_parse_member(bundle, "torque", behavior.torque, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &DampingBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(DampingBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Float>("linear_damping").min(0.0f);
    b.add<decl::Float>("angular_damping").min(0.0f);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}
std::optional<DampingBundle> DampingBundle::parse(const Bundle &bundle,
                                                  BundleParseErrors &r_errors)
{
  DampingBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "linear_damping", behavior.linear_damping, r_errors);
  bundle_parse_member(bundle, "angular_damping", behavior.angular_damping, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &RigidBodyInstancesBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(RigidBodyInstancesBundle::name);
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

std::optional<RigidBodyInstancesBundle> RigidBodyInstancesBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  RigidBodyInstancesBundle behavior;
  bundle_parse_member(bundle, "instances", behavior.instances_geometry, r_errors);
  bundle_parse_member(bundle, "collision_shape_type", behavior.collision_shape_type, r_errors);
  bundle_parse_member(bundle, "motion_type", behavior.motion_type, r_errors);
  bundle_parse_member(bundle, "friction", behavior.friction, r_errors);
  bundle_parse_member(bundle, "bounciness", behavior.bounciness, r_errors);
  bundle_parse_member(bundle, "density", behavior.density, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  behavior.instances_geometry.keep_only({bke::GeometryComponent::Type::Instance});
  return behavior;
}

const FlatBundleTypePtr &SoftBodyMeshBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(SoftBodyMeshBundle::name);
    b.add<decl::Geometry>("mesh").supported_type(bke::GeometryComponent::Type::Mesh);
    b.add<decl::Float>("stretch_stiffness").default_value(1e6f).min(0.0f).supports_field();
    b.add<decl::Float>("bend_stiffness").default_value(1e6f).min(0.0f).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<SoftBodyMeshBundle> SoftBodyMeshBundle::parse(const Bundle &bundle,
                                                            BundleParseErrors &r_errors)
{
  SoftBodyMeshBundle behavior;
  bundle_parse_member(bundle, "mesh", behavior.mesh_geometry, r_errors);
  bundle_parse_member(bundle, "stretch_stiffness", behavior.stretch_stiffness, r_errors);
  bundle_parse_member(bundle, "bend_stiffness", behavior.bend_stiffness, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  behavior.mesh_geometry.keep_only({bke::GeometryComponent::Type::Mesh});
  return behavior;
}

const FlatBundleTypePtr &XPBDGeometryBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(XPBDGeometryBundle::name);
    b.add<decl::Geometry>("geometry").supported_type(bke::GeometryComponent::Type::Mesh);
    b.add<decl::Float>("mass").default_value(1.0f).min(0.0f);
    b.add<decl::Float>("friction").default_value(0.5f).min(0.0f);
    b.add<decl::Bool>("has_rotation").default_value(false);
    b.add<decl::Rotation>("initial_rotation").supports_field();
    b.add<decl::String>("output_rotation_name");
    b.add<decl::Vector>("inertia").default_value(float3(1.0f)).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<XPBDGeometryBundle> XPBDGeometryBundle::parse(const Bundle &bundle,
                                                            BundleParseErrors &r_errors)
{
  XPBDGeometryBundle behavior;
  bundle_parse_member(bundle, "geometry", behavior.geometry, r_errors);
  bundle_parse_member(bundle, "mass", behavior.mass, r_errors);
  bundle_parse_member(bundle, "friction", behavior.friction, r_errors);
  bundle_parse_member(bundle, "has_rotation", behavior.has_rotation, r_errors);
  bundle_parse_member(bundle, "initial_rotation", behavior.initial_rotations, r_errors);
  bundle_parse_member(bundle, "output_rotation_name", behavior.output_rotation_name, r_errors);
  bundle_parse_member(bundle, "inertia", behavior.inertia, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &EdgeLengthXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(EdgeLengthXPBDConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Float>("compliance").min(0.0f).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<EdgeLengthXPBDConstraintBundle> EdgeLengthXPBDConstraintBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  EdgeLengthXPBDConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "selection", behavior.selection, r_errors);
  bundle_parse_member(bundle, "compliance", behavior.compliance, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &CurveSegmentXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(CurveSegmentXPBDConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Float>("compliance").default_value(true).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<CurveSegmentXPBDConstraintBundle> CurveSegmentXPBDConstraintBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  CurveSegmentXPBDConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "compliance", behavior.compliance, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &PinnedPositionXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(PinnedPositionXPBDConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Vector>("position").supports_field();
    b.add<decl::Float>("compliance").min(0.0f).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<PinnedPositionXPBDConstraintBundle> PinnedPositionXPBDConstraintBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  PinnedPositionXPBDConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "selection", behavior.selection, r_errors);
  bundle_parse_member(bundle, "position", behavior.position, r_errors);
  bundle_parse_member(bundle, "compliance", behavior.compliance, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &PinnedRotationXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(PinnedRotationXPBDConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Rotation>("rotation").supports_field();
    b.add<decl::Float>("compliance").min(0.0f).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<PinnedRotationXPBDConstraintBundle> PinnedRotationXPBDConstraintBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  PinnedRotationXPBDConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "selection", behavior.selection, r_errors);
  bundle_parse_member(bundle, "rotation", behavior.rotation, r_errors);
  bundle_parse_member(bundle, "compliance", behavior.compliance, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &PressureXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(PressureXPBDConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Float>("pressure").default_value(2.0f).min(0.0f);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<PressureXPBDConstraintBundle> PressureXPBDConstraintBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  PressureXPBDConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "pressure", behavior.pressure, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &InfiniteGroundPlaneBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(InfiniteGroundPlaneBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Vector>("position");
    b.add<decl::Vector>("normal");
    b.add<decl::Float>("friction").default_value(0.5f).min(0.0f);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<InfiniteGroundPlaneBundle> InfiniteGroundPlaneBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  InfiniteGroundPlaneBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "position", behavior.position, r_errors);
  bundle_parse_member(bundle, "normal", behavior.normal, r_errors);
  bundle_parse_member(bundle, "friction", behavior.friction, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &SphericalSelfCollisionXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(SphericalSelfCollisionXPBDConstraintBundle::name);
    b.add<decl::String>("filter");
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<SphericalSelfCollisionXPBDConstraintBundle>
SphericalSelfCollisionXPBDConstraintBundle::parse(const Bundle &bundle,
                                                  BundleParseErrors &r_errors)
{
  SphericalSelfCollisionXPBDConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &RodStretchAndShearXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(RodStretchAndShearXPBDConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Float>("compliance").default_value(1e-4f).min(0.0f);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<RodStretchAndShearXPBDConstraintBundle> RodStretchAndShearXPBDConstraintBundle::
    parse(const Bundle &bundle, BundleParseErrors &r_errors)
{
  RodStretchAndShearXPBDConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "compliance", behavior.compliance, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &RodBendAndTwistXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(RodBendAndTwistXPBDConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Float>("compliance").default_value(1e-4f).min(0.0f);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<RodBendAndTwistXPBDConstraintBundle> RodBendAndTwistXPBDConstraintBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  RodBendAndTwistXPBDConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "compliance", behavior.compliance, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &AlignPositionsConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(AlignPositionsConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Int>("group_id").supports_field();
    b.add<decl::Float>("compliance").min(0.0f).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<AlignPositionsConstraintBundle> AlignPositionsConstraintBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  AlignPositionsConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "selection", behavior.selection, r_errors);
  bundle_parse_member(bundle, "group_id", behavior.group_id, r_errors);
  bundle_parse_member(bundle, "compliance", behavior.compliance, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &AttachUVSurfaceConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(AttachUVSurfaceConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::String>("mesh_path");
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Vector>("uv_map").supports_field();
    b.add<decl::Vector>("sample_uv").supports_field();
    b.add<decl::Float>("compliance").min(0.0f).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<AttachUVSurfaceConstraintBundle> AttachUVSurfaceConstraintBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  AttachUVSurfaceConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "mesh_path", behavior.mesh_path, r_errors);
  bundle_parse_member(bundle, "selection", behavior.selection, r_errors);
  bundle_parse_member(bundle, "compliance", behavior.compliance, r_errors);

  const bke::DataTypeConversions &conversions = bke::get_implicit_type_conversions();
  fn::Field<float3> uv_map;
  bundle_parse_member(bundle, "uv_map", uv_map, r_errors);
  behavior.uv_map = conversions.try_convert(uv_map, CPPType::get<float2>());
  fn::Field<float3> sample_uv;
  bundle_parse_member(bundle, "sample_uv", sample_uv, r_errors);
  behavior.sample_uv = conversions.try_convert(sample_uv, CPPType::get<float2>());

  if (r_errors.has_error() || !behavior.uv_map || !behavior.sample_uv) {
    return std::nullopt;
  }
  return behavior;
}

const FlatBundleTypePtr &DistanceBasedEdgeBendingConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(DistanceBasedEdgeBendingConstraintBundle::name);
    b.add<decl::String>("filter");
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Float>("compliance").min(0.0f).supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<DistanceBasedEdgeBendingConstraintBundle> DistanceBasedEdgeBendingConstraintBundle::
    parse(const Bundle &bundle, BundleParseErrors &r_errors)
{
  DistanceBasedEdgeBendingConstraintBundle behavior;
  bundle_parse_member(bundle, "filter", behavior.filter, r_errors);
  bundle_parse_member(bundle, "selection", behavior.selection, r_errors);
  bundle_parse_member(bundle, "compliance", behavior.compliance, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return behavior;
}

}  // namespace blender::nodes::physics_bundles
