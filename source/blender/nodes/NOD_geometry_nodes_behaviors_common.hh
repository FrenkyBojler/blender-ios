/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set.hh"
#include "FN_field.hh"

#include "NOD_geometry_nodes_behaviors.hh"

namespace blender::nodes {

class GravityBehavior : public BehaviorCommon {
 public:
  static constexpr StringRefNull type = "COMMON_GRAVITY";

  float3 gravity;

  static const std::shared_ptr<BehaviorDef> &def();
  static std::optional<GravityBehavior> parse(const Bundle &bundle, BehaviorParseErrors &r_errors);
};

class ForceBehavior : public BehaviorCommon {
 public:
  static constexpr StringRefNull type = "COMMON_FORCE";

  std::string filter;
  fn::Field<bool> selection;
  fn::Field<float3> force;

  static const std::shared_ptr<BehaviorDef> &def();
  static std::optional<ForceBehavior> parse(const Bundle &bundle, BehaviorParseErrors &r_errors);
};

class RigidBodyInstancesBehavior : public BehaviorCommon {
 public:
  static constexpr StringRefNull type = "COMMON_RIGID_BODY_INSTANCES";

  bke::GeometrySet instances_geometry;
  fn::Field<int> collision_shape_type;
  fn::Field<int> motion_type;
  fn::Field<float> friction;
  fn::Field<float> bounciness;
  fn::Field<float> density;

  static const std::shared_ptr<BehaviorDef> &def();
  static std::optional<RigidBodyInstancesBehavior> parse(const Bundle &bundle,
                                                         BehaviorParseErrors &r_errors);
};

class SoftBodyMeshBehavior : public BehaviorCommon {
 public:
  static constexpr StringRefNull type = "COMMON_SOFT_BODY_MESH";

  bke::GeometrySet mesh_geometry;
  fn::Field<float> stretch_stiffness;
  fn::Field<float> bend_stiffness;

  static const std::shared_ptr<BehaviorDef> &def();
  static std::optional<SoftBodyMeshBehavior> parse(const Bundle &bundle,
                                                   BehaviorParseErrors &r_errors);
};

class RigidBodyConstraintDistance : public BehaviorCommon {
 public:
  static constexpr StringRefNull type = "COMMON_RIGID_BODY_CONSTRAINT_DISTANCE";

  std::string bodies_a;
  std::string bodies_b;
  ListPtr ids_a;
  ListPtr ids_b;

  static const std::shared_ptr<BehaviorDef> &def();
  static std::optional<RigidBodyConstraintDistance> parse(const Bundle &bundle,
                                                          BehaviorParseErrors &r_errors);
};

}  // namespace blender::nodes
