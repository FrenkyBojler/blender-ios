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

class TorqueBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.Torque";

  std::string filter;
  fn::Field<bool> selection;
  fn::Field<float3> torque;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<TorqueBundle> parse(const Bundle &bundle, BundleParseErrors &r_errors);
};

class DampingBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.Damping";
  std::string filter;
  float linear_damping;
  float angular_damping;
  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<DampingBundle> parse(const Bundle &bundle, BundleParseErrors &r_errors);
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
  fn::Field<float> friction;
  bool has_rotation;
  std::string output_rotation_name;
  fn::Field<math::Quaternion> initial_rotations;
  fn::Field<float3> inertia;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<XPBDGeometryBundle> parse(const Bundle &bundle,
                                                 BundleParseErrors &r_errors);
};

class EdgeLengthXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.EdgeLengthXPBDConstraint";

  std::string filter;
  fn::Field<bool> selection;
  fn::Field<float> compliance;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<EdgeLengthXPBDConstraintBundle> parse(const Bundle &bundle,
                                                             BundleParseErrors &r_errors);
};

class CurveSegmentXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.CurveSegmentXPBDConstraint";

  std::string filter;
  fn::Field<float> compliance;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<CurveSegmentXPBDConstraintBundle> parse(const Bundle &bundle,
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

class PinnedRotationXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.PinnedRotationXPBDConstraint";

  std::string filter;
  fn::Field<bool> selection;
  fn::Field<math::Quaternion> rotation;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<PinnedRotationXPBDConstraintBundle> parse(const Bundle &bundle,
                                                                 BundleParseErrors &r_errors);
};

class OverpressureXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.OverpressureXPBDConstraint";

  std::string filter;
  float overpressure;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<OverpressureXPBDConstraintBundle> parse(const Bundle &bundle,
                                                               BundleParseErrors &r_errors);
};

class InfiniteGroundPlaneBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.InfiniteGroundPlane";

  std::string filter;
  float3 position;
  float3 normal;
  float friction;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<InfiniteGroundPlaneBundle> parse(const Bundle &bundle,
                                                        BundleParseErrors &r_errors);
};

class SphericalSelfCollisionXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.SphericalSelfCollisionXPBDConstraint";

  std::string filter;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<SphericalSelfCollisionXPBDConstraintBundle> parse(
      const Bundle &bundle, BundleParseErrors &r_errors);
};

class RodStretchAndShearXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.RodStretchAndShearXPBDConstraint";

  std::string filter;
  fn::Field<float> compliance;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<RodStretchAndShearXPBDConstraintBundle> parse(const Bundle &bundle,
                                                                     BundleParseErrors &r_errors);
};

class RodBendAndTwistXPBDConstraintBundle : public NestedBundleCommon {
 public:
  static constexpr StringRefNull name = "Blender.RodBendAndTwistXPBDConstraint";

  std::string filter;
  fn::Field<float> compliance;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<RodBendAndTwistXPBDConstraintBundle> parse(const Bundle &bundle,
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
