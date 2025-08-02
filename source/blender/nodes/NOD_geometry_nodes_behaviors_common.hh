/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set.hh"

#include "FN_field.hh"

#include "NOD_bundle_type_fwd.hh"
#include "NOD_geometry_nodes_behaviors.hh"

namespace blender::nodes {

class GravityBehavior : public BehaviorCommon {
 public:
  static constexpr StringRefNull name = "Blender.Gravity";

  float3 gravity;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<GravityBehavior> parse(const Bundle &bundle, BehaviorParseErrors &r_errors);
};

class ForceBehavior : public BehaviorCommon {
 public:
  static constexpr StringRefNull name = "Blender.Force";

  std::string filter;
  fn::Field<bool> selection;
  fn::Field<float3> force;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<ForceBehavior> parse(const Bundle &bundle, BehaviorParseErrors &r_errors);
};

class RigidBodyInstancesBehavior : public BehaviorCommon {
 public:
  static constexpr StringRefNull name = "Blender.RigidBodyInstances";

  bke::GeometrySet instances_geometry;
  fn::Field<int> collision_shape_type;
  fn::Field<int> motion_type;
  fn::Field<float> friction;
  fn::Field<float> bounciness;
  fn::Field<float> density;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<RigidBodyInstancesBehavior> parse(const Bundle &bundle,
                                                         BehaviorParseErrors &r_errors);
};

class SoftBodyMeshBehavior : public BehaviorCommon {
 public:
  static constexpr StringRefNull name = "Blender.SoftBodyMesh";

  bke::GeometrySet mesh_geometry;
  fn::Field<float> stretch_stiffness;
  fn::Field<float> bend_stiffness;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<SoftBodyMeshBehavior> parse(const Bundle &bundle,
                                                   BehaviorParseErrors &r_errors);
};

}  // namespace blender::nodes
