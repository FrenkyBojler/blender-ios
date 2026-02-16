/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_bundle_type.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"
#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

namespace blender::nodes::physics_bundles {

static void add_filter(FlatBundleTypeBuilder &b)
{
  b.add<decl::String>("filter");
  b.add<decl::Bool>("filter_local").default_value(false);
}

const FlatBundleTypePtr &GravityBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(GravityBundle::name);
    add_filter(b);
    b.add<decl::Vector>("gravity").default_value(float3(0.0f, 0.0f, -9.81f));
    FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &ForceBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(ForceBundle::name);
    add_filter(b);
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Vector>("force").supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &TorqueBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(TorqueBundle::name);
    add_filter(b);
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Vector>("torque").supports_field();
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &ColliderBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(ColliderBundle::name);
    add_filter(b);
    b.add<decl::Geometry>("geometry");
    b.add<decl::Float>("friction").min(0.0f);
    b.add<decl::Float>("compliance").min(0.0f);
    b.add<decl::Bool>("deforming").default_value(false);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &DampingBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(DampingBundle::name);
    add_filter(b);
    b.add<decl::Float>("linear_damping").min(0.0f);
    b.add<decl::Float>("angular_damping").min(0.0f);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &XPBDGeometryBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(XPBDGeometryBundle::name);
    b.add<decl::Geometry>("geometry").supported_type(bke::GeometryComponent::Type::Mesh);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &PinnedPositionXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(PinnedPositionXPBDConstraintBundle::name);
    add_filter(b);
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Vector>("position").supports_field();
    b.add<decl::Float>("compliance").min(0.0f).supports_field();
    b.add<decl::String>("was_pinned_attribute");
    b.add<decl::String>("previous_pin_position_attribute");
    b.add<decl::String>("lambda_attribute");
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &PinnedRotationXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(PinnedRotationXPBDConstraintBundle::name);
    add_filter(b);
    b.add<decl::Bool>("selection").default_value(true).supports_field();
    b.add<decl::Rotation>("rotation").supports_field();
    b.add<decl::Float>("compliance").min(0.0f).supports_field();
    b.add<decl::String>("was_pinned_attribute");
    b.add<decl::String>("previous_pin_rotation_attribute");
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &InfiniteGroundPlaneBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(InfiniteGroundPlaneBundle::name);
    add_filter(b);
    b.add<decl::Vector>("position");
    b.add<decl::Vector>("normal").default_value(float3(0.0f, 0.0f, 1.0f));
    b.add<decl::Float>("friction").default_value(0.5f).min(0.0f);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &RodStretchAndShearXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(RodStretchAndShearXPBDConstraintBundle::name);
    add_filter(b);
    b.add<decl::Float>("compliance").default_value(1e-4f).min(0.0f);
    b.add<decl::String>("lambda_position_attribute");
    b.add<decl::String>("lambda_rotation_attribute");
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

const FlatBundleTypePtr &RodBendAndTwistXPBDConstraintBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(RodBendAndTwistXPBDConstraintBundle::name);
    add_filter(b);
    b.add<decl::Float>("compliance").default_value(1e-4f).min(0.0f);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

}  // namespace blender::nodes::physics_bundles
