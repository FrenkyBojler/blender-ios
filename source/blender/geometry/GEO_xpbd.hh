/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_mutex.hh"
#include "BLI_vector.hh"

#include "NOD_geometry_nodes_bundle_fwd.hh"

#include "FN_field.hh"

namespace blender::geometry::xpbd_old {

class ConstraintCorrections;
class PhysicsState;
class ConstraintContext;

class ForceField {
 public:
  std::string self_path;
  std::string filter;
  fn::Field<float3> force_field;
};

class AccelerationField {
 public:
  std::string self_path;
  std::string filter;
  fn::Field<float3> acceleration_field;
};

class Damping {
 public:
  std::string self_path;
  std::string filter;
  float linear_damping;
  float angular_damping;
};

struct SimGeometrySet {
  std::string path;
  bke::GeometrySet geometry;
  std::string mass_attribute;
  std::string inertia_attribute;
  std::string rotation_attribute;
  std::string velocity_attribute;
  std::string angular_velocity_attribute;
  fn::Field<float> friction;
  fn::Field<float> bounciness;

  mutable Mutex extra_mutex;
  nodes::BundlePtr extra;

  nodes::Bundle &extra_for_write();

  template<typename T> void set_extra(const StringRef key, T value);
  void remove_extra(const StringRef key);
  template<typename T> std::optional<T> get_extra(const StringRef key) const;
};

struct SimGeometry {
  using GeometryVariant = std::variant<Mesh *, PointCloud *, Curves *>;
  GeometryVariant data;
  SimGeometrySet &src;
  float quantize_scale = 1'000'000.0f;

  SimGeometry(SimGeometrySet &src, GeometryVariant data);

  std::optional<bke::AttributeAccessor> attributes() const;
  std::optional<bke::MutableAttributeAccessor> attributes_for_write();
  int points_num() const;

  int set_point_field_context(std::optional<bke::GeometryFieldContext> &r_context) const;
};

struct PositionCorrection {
  Mutex mutex;
  int3 offset = {0, 0, 0};
  int num_corrections = 0;
};

struct RotationCorrection {
  Mutex mutex;
  int4 offset = {0, 0, 0, 0};
  int num_corrections = 0;
};

class LocalConstraintCorrections {
  ConstraintCorrections &corrections_;

 public:
  LocalConstraintCorrections(ConstraintCorrections &corrections);
  void add_position_correction(int geometry_i, int position_i, const float3 &offset);
  void add_rotation_correction(int geometry_i, int position_i, const math::Quaternion &offset);
};

class ConstraintCorrections {
 private:
  MutableSpan<SimGeometry> sim_geometries_;

  struct SimGeometryCorrections {
    Array<PositionCorrection> position_corrections;
    Array<RotationCorrection> rotation_corrections;
  };
  Array<SimGeometryCorrections> corrections_;
  LocalConstraintCorrections local_corrections_;

  friend LocalConstraintCorrections;

 public:
  ConstraintCorrections(MutableSpan<SimGeometry> sim_geometries);
  LocalConstraintCorrections &local();
  void apply();
};

class ConstraintSetSolveParams {
 public:
  float delta_time;
  Span<SimGeometry> sim_geometries;
  const PhysicsState *physics_state;
  ConstraintCorrections &corrections;
};

class ConstraintSet {
 public:
  virtual ~ConstraintSet() = default;
  virtual void ensure_init(const ConstraintContext &context,
                           MutableSpan<SimGeometry> sim_geometries);
  virtual void solve(ConstraintSetSolveParams &params);
  virtual void post_solve_apply(MutableSpan<SimGeometry> sim_geometries,
                                const PhysicsState *physics_state);
};

class RigidBodyInstances {
 public:
  enum class CollisionShapeType {
    Box = 0,
    Sphere = 1,
    ConvexHull = 2,
  };

  enum class MotionType {
    Dynamic = 0,
    Static = 1,
    Animated = 2,
  };

  std::string self_path;
  bke::GeometrySet instances_geometry;
  /** Uses #CollisionShapeType. */
  fn::Field<int> collision_shape_type;
  /** Uses #MotionType. */
  fn::Field<int> motion_type;
  fn::Field<float> friction;
  fn::Field<float> bounciness;
  fn::Field<float> density;
};

class SoftBodyMesh {
 public:
  std::string self_path;
  bke::GeometrySet mesh_geometry;
  fn::Field<float> stretch_stiffness;
  fn::Field<float> bend_stiffness;
};

/* Internal physics engine state. */
class PhysicsState {
 public:
  virtual ~PhysicsState();

  static PhysicsState *create();
};

struct Behaviors {
  Vector<SimGeometrySet *> sim_geometry_sets;
  Vector<ForceField> force_fields;
  Vector<AccelerationField> acceleration_fields;
  Vector<Damping> dampings;
  Vector<ConstraintSet *> constraint_sets;
  Vector<RigidBodyInstances> rigid_body_instances;
  Vector<SoftBodyMesh> soft_body_meshes;
  PhysicsState *physics_state = nullptr;
  int update_counter = 0;
};

void solve(Behaviors &behaviors, float delta_time, int substeps);

ConstraintSet &create_constraint__edge_lengths(ResourceScope &scope,
                                               std::string self_path,
                                               std::string filter,
                                               std::string rest_length_attribute,
                                               float compliance);
ConstraintSet &create_constraint__curve_lengths(ResourceScope &scope,
                                                std::string self_path,
                                                std::string filter,
                                                std::string rest_length_attribute,
                                                float compliance);
ConstraintSet &create_constraint__cosserat_rod_lengths(ResourceScope &scope,
                                                       std::string self_path,
                                                       std::string filter,
                                                       std::string rest_length_attribute,
                                                       float compliance);
ConstraintSet &create_constraint__cosserat_rod_bending(ResourceScope &scope,
                                                       std::string self_path,
                                                       std::string filter,
                                                       std::string rest_length_attribute,
                                                       std::string rest_shape_attribute,
                                                       float compliance);
ConstraintSet &create_constraint__fixed_positions(ResourceScope &scope,
                                                  std::string self_path,
                                                  std::string filter,
                                                  fn::Field<bool> selection_field,
                                                  fn::Field<float3> fixed_positions_field);
ConstraintSet &create_constraint__fixed_rotations(
    ResourceScope &scope,
    std::string self_path,
    std::string filter,
    fn::Field<bool> selection_field,
    fn::Field<math::Quaternion> fixed_rotations_field,
    float compliance);
ConstraintSet &create_constraint__infinite_collision_plane(ResourceScope &scope,
                                                           std::string self_path,
                                                           std::string filter,
                                                           const float3 &position,
                                                           const float3 &normal);
ConstraintSet &create_constraint__global_volume(ResourceScope &scope,
                                                std::string self_path,
                                                std::string filter,
                                                std::string rest_volume_name,
                                                float overpressure = 1.0f);
ConstraintSet &create_constraint__collision(ResourceScope &scope,
                                            std::string self_path,
                                            std::string filter,
                                            fn::Field<bool> selection_field,
                                            fn::Field<float> radius_field,
                                            float speculative_contact_distance);

inline void LocalConstraintCorrections::add_position_correction(const int geometry_i,
                                                                const int position_i,
                                                                const float3 &offset)
{
  if (position_i < 0) {
    return;
  }
  const float quantize_scale = corrections_.sim_geometries_[geometry_i].quantize_scale;
  const int3 quantized_offset = int3(offset * quantize_scale);
  ConstraintCorrections::SimGeometryCorrections &corrections =
      corrections_.corrections_[geometry_i];
  PositionCorrection &correction = corrections.position_corrections[position_i];
  std::lock_guard lock(correction.mutex);
  correction.offset += quantized_offset;
  correction.num_corrections++;
}

inline void LocalConstraintCorrections::add_rotation_correction(const int geometry_i,
                                                                const int rotation_i,
                                                                const math::Quaternion &offset)
{
  if (rotation_i < 0) {
    return;
  }
  const float quantize_scale = corrections_.sim_geometries_[geometry_i].quantize_scale;
  /* TODO Double-cover problem: quaternions q and -q describe the same rotation, which can cause
   * rapid flip when simply adding deltas. May have to flip quaternions to push towards the nearest
   * pole consistently. */
  const int4 quantized_offset = int4(float4(offset) * quantize_scale);
  ConstraintCorrections::SimGeometryCorrections &corrections =
      corrections_.corrections_[geometry_i];
  RotationCorrection &correction = corrections.rotation_corrections[rotation_i];
  std::lock_guard lock(correction.mutex);
  correction.offset += quantized_offset;
  correction.num_corrections++;
}

}  // namespace blender::geometry::xpbd_old
