/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set.hh"

#include "FN_field.hh"

#include "NOD_bundle_type_fwd.hh"
#include "NOD_geometry_nodes_bundle_fwd.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"

namespace blender::nodes::physics_bundles {

class GravityBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.Gravity";

  float3 gravity;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<GravityBundle> parse(const Bundle &bundle, BundleParseErrors &r_errors);
};

class ForceBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.Force";

  std::string filter;
  fn::Field<bool> selection;
  fn::Field<float3> force;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<ForceBundle> parse(const Bundle &bundle, BundleParseErrors &r_errors);
};

enum class RigidBodyCollisionShapeType {
  Box,
  Sphere,
  ConvexHull,
};

enum class RigidBodyMotionType {
  Dynamic,
  Static,
  Animated,
};

class RigidBodyInstancesBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.RigidBodyInstances";

  bke::GeometrySet instances_geometry;
  /** Uses #RigidBodyCollisionShapeType. */
  fn::Field<int> collision_shape_type;
  /** Uses #RigidBodyMotionType. */
  fn::Field<int> motion_type;
  fn::Field<float> friction;
  fn::Field<float> bounciness;
  fn::Field<float> density;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<RigidBodyInstancesBundle> parse(const Bundle &bundle,
                                                       BundleParseErrors &r_errors);

  static std::optional<RigidBodyCollisionShapeType> parse_collision_shape_type(const int type);
  static std::optional<RigidBodyMotionType> parse_motion_type(const int type);
};

class SoftBodyMeshBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.SoftBodyMesh";

  bke::GeometrySet mesh_geometry;
  fn::Field<float> stretch_stiffness;
  fn::Field<float> bend_stiffness;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<SoftBodyMeshBundle> parse(const Bundle &bundle,
                                                 BundleParseErrors &r_errors);
};

class XPBDGeometryBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.XPBDGeometry";
  bke::GeometrySet geometry;
  fn::Field<float> mass;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<XPBDGeometryBundle> parse(const Bundle &bundle,
                                                 BundleParseErrors &r_errors);
};

class EdgeLengthXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.EdgeLengthXPBDConstraint";

  std::string filter;
  fn::Field<bool> selection;
  fn::Field<float> length;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<EdgeLengthXPBDConstraintBundle> parse(const Bundle &bundle,
                                                             BundleParseErrors &r_errors);
};

class PinnedPositionXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.PinnedPositionXPBDConstraint";

  std::string filter;
  fn::Field<bool> selection;
  fn::Field<float3> position;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<PinnedPositionXPBDConstraintBundle> parse(const Bundle &bundle,
                                                                 BundleParseErrors &r_errors);
};

inline std::optional<RigidBodyCollisionShapeType> RigidBodyInstancesBundle::
    parse_collision_shape_type(const int type)
{
  switch (type) {
    case 0:
      return RigidBodyCollisionShapeType::Box;
    case 1:
      return RigidBodyCollisionShapeType::Sphere;
    case 2:
      return RigidBodyCollisionShapeType::ConvexHull;
  }
  return std::nullopt;
}

inline std::optional<RigidBodyMotionType> RigidBodyInstancesBundle::parse_motion_type(
    const int type)
{
  switch (type) {
    case 0:
      return RigidBodyMotionType::Dynamic;
    case 1:
      return RigidBodyMotionType::Static;
    case 2:
      return RigidBodyMotionType::Animated;
  }
  return std::nullopt;
}

}  // namespace blender::nodes::physics_bundles
