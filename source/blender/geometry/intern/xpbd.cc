/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_bounds.hh"
#include "BLI_math_rotation.hh"

#include "BKE_curves.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_instances.hh"
#include "BKE_pointcloud.hh"

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "GEO_join_geometries.hh"
#include "GEO_jolt.hh"
#include "GEO_shape_hash.hh"
#include "GEO_xpbd.hh"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"

#include "Jolt/Jolt.h"
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsStepListener.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/RegisterTypes.h>

#include <iostream>

/* TODO This should become a shared API with the Jolt solver node. */
namespace blender::geometry::jolt_physics {

static JPH::Vec3 convert_vec3(const float3 &v)
{
  return JPH::Vec3(v.x, v.y, v.z);
}
static float3 convert_vec3(const JPH::Vec3 &v)
{
  return float3(v.GetX(), v.GetY(), v.GetZ());
}
static JPH::Quat convert_quat(const math::Quaternion &q)
{
  return JPH::Quat(q.x, q.y, q.z, q.w);
}
static math::Quaternion convert_quat(const JPH::Quat &q)
{
  return math::Quaternion(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

namespace ObjectLayers {
static constexpr JPH::ObjectLayer non_moving(0);
static constexpr JPH::ObjectLayer moving(1);
static constexpr int num_layers = 2;
}  // namespace ObjectLayers

namespace BroadPhaseLayers {
static constexpr JPH::BroadPhaseLayer non_moving(0);
static constexpr JPH::BroadPhaseLayer moving(1);
static constexpr int num_layers = 2;
}  // namespace BroadPhaseLayers

class BroadPhaseLayerInterfaceImpl : public JPH::BroadPhaseLayerInterface {
 private:
  std::array<JPH::BroadPhaseLayer, ObjectLayers::num_layers> object_to_broad_phase_;

 public:
  BroadPhaseLayerInterfaceImpl()
  {
    object_to_broad_phase_[ObjectLayers::non_moving] = BroadPhaseLayers::non_moving;
    object_to_broad_phase_[ObjectLayers::moving] = BroadPhaseLayers::moving;
  }

  uint GetNumBroadPhaseLayers() const override
  {
    return BroadPhaseLayers::num_layers;
  }

  JPH::BroadPhaseLayer GetBroadPhaseLayer(const JPH::ObjectLayer object_layer) const override
  {
    BLI_assert(object_layer >= 0 && object_layer < ObjectLayers::num_layers);
    return object_to_broad_phase_[object_layer];
  }
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(const JPH::ObjectLayer a, const JPH::ObjectLayer b) const override
  {
    if (a == ObjectLayers::moving || b == ObjectLayers::moving) {
      return true;
    }
    return false;
  }
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(const JPH::ObjectLayer object_layer,
                     const JPH::BroadPhaseLayer broad_phase_layer) const override
  {
    if (object_layer == ObjectLayers::moving || broad_phase_layer == BroadPhaseLayers::moving) {
      return true;
    }
    return false;
  }
};

class ContactListenerImpl : public JPH::ContactListener {
 public:
  virtual JPH::ValidateResult OnContactValidate(
      const JPH::Body & /*body_a*/,
      const JPH::Body & /*body_b*/,
      JPH::RVec3Arg /*in_base_offset*/,
      const JPH::CollideShapeResult & /*in_collision_result*/) override
  {
    return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
  }

  void OnContactAdded(const JPH::Body & /*body_a*/,
                      const JPH::Body & /*body_b*/,
                      const JPH::ContactManifold & /*manifold*/,
                      JPH::ContactSettings & /*settings*/) override
  {
  }

  void OnContactPersisted(const JPH::Body & /*body_a*/,
                          const JPH::Body & /*body_b*/,
                          const JPH::ContactManifold & /*manifold*/,
                          JPH::ContactSettings & /*settings*/) override
  {
  }

  void OnContactRemoved(const JPH::SubShapeIDPair & /*sub_shape_pair*/) override {}
};

class BodyActivationListenerImpl : public JPH::BodyActivationListener {
 public:
  void OnBodyActivated(const JPH::BodyID & /*body_id*/, uint64_t /*body_user_data*/) override {}

  void OnBodyDeactivated(const JPH::BodyID & /*body_id*/, uint64_t /*body_user_data*/) override {}
};

struct JoltRigidBody {
  JPH::Body *body = nullptr;
};

struct JoltRigidBodies {
  Map<int, JoltRigidBody> bodies_by_id;
};

struct JoltSoftBody {
  JPH::Body *body = nullptr;
};

struct CollisionShapeParams {
  const bke::GeometrySet &geometry;
  const float3 &scale;
  const float density;
};

class CollisionShapeCache {
 private:
  using ShapeID = const void *;

  /* Switch to translated shape if the center-of-mass differs from the origin. */
  static constexpr float com_offset_threshold = 1e-5f;
  /* Default padding around collision shapes to avoid penetration.
   * Collision becomes more expensive when objects are penetrating, the convex radius should
   * prevent this in most cases.
   * TODO make this a configurable option, allow shrinking of collision shapes accordingly to align
   * with visual shapes. */
  static constexpr float default_convex_radius = 0.05f;

  struct BoxID {
    float3 half_extent;
    float density;

    uint64_t hash() const
    {
      return get_default_hash(this->half_extent, this->density);
    }

    BLI_STRUCT_EQUALITY_OPERATORS_2(BoxID, half_extent, density)
  };

  struct SphereID {
    float radius;
    float density;

    uint64_t hash() const
    {
      return get_default_hash(this->radius, this->density);
    }

    BLI_STRUCT_EQUALITY_OPERATORS_2(SphereID, radius, density)
  };

  struct ConvexHullID {
    geometry::GeometryShapeHash shape_hash;
    float3 scale;
    float density;

    uint64_t hash() const
    {
      return get_default_hash(this->shape_hash, this->scale, this->density);
    }

    BLI_STRUCT_EQUALITY_OPERATORS_3(ConvexHullID, shape_hash, scale, density)
  };

  struct CompoundID {
    struct SubShapeID {
      ShapeID shape_id;
      float3 position;
      math::Quaternion rotation;

      uint64_t hash() const
      {
        return get_default_hash(shape_id, position, rotation);
      }

      BLI_STRUCT_EQUALITY_OPERATORS_3(SubShapeID, shape_id, position, rotation)
    };

    Array<SubShapeID> sub_shape_ids;

    uint64_t hash() const
    {
      uint64_t hash = 0;
      for (const SubShapeID &sub_id : sub_shape_ids) {
        hash = get_default_hash(hash, sub_id);
      }
      return hash;
    }

    friend bool operator==(const CompoundID &a, const CompoundID &b)
    {
      if (a.sub_shape_ids.size() != b.sub_shape_ids.size()) {
        return false;
      }
      for (const int i : a.sub_shape_ids.index_range()) {
        if (a.sub_shape_ids[i] != b.sub_shape_ids[i]) {
          return false;
        }
      }
      return true;
    }

    friend bool operator!=(const CompoundID &a, const CompoundID &b)
    {
      return !(a == b);
    }
  };

  struct DecoratedID {
    ShapeID shape_id;
    float4x4 transform;

    uint64_t hash() const
    {
      return get_default_hash(this->shape_id, this->transform);
    }

    BLI_STRUCT_EQUALITY_OPERATORS_2(DecoratedID, shape_id, transform)
  };

  using ShapeIDVariant = std::variant<BoxID, SphereID, ConvexHullID, CompoundID, DecoratedID>;
  using CachedShape = JPH::ShapeSettings::ShapeResult;

  Map<BoxID, CachedShape> boxes;
  Map<SphereID, CachedShape> spheres;
  Map<ConvexHullID, CachedShape> convex_hulls;
  Map<CompoundID, CachedShape> compounds;
  Map<DecoratedID, CachedShape> decorated_shapes;

  static bool is_used(const CachedShape &cached_shape)
  {
    if (cached_shape.IsValid()) {
      return bool(cached_shape.Get()->GetUserData());
    }
    return false;
  }

  static void set_used(const CachedShape &cached_shape, bool used = true)
  {
    if (cached_shape.IsValid()) {
      cached_shape.Get()->SetUserData(uint64_t(used));
    }
  }

  static void clear_used(const CachedShape &cached_shape)
  {
    set_used(cached_shape, false);
  }

 public:
  void reset_used()
  {
    for (CachedShape &cached_shape : this->boxes.values()) {
      clear_used(cached_shape);
    }
    for (CachedShape &cached_shape : this->spheres.values()) {
      clear_used(cached_shape);
    }
    for (CachedShape &cached_shape : this->convex_hulls.values()) {
      clear_used(cached_shape);
    }
    for (CachedShape &cached_shape : this->compounds.values()) {
      clear_used(cached_shape);
    }
    for (CachedShape &cached_shape : this->decorated_shapes.values()) {
      clear_used(cached_shape);
    }
  }

  void remove_unused()
  {
    this->boxes.remove_if([](const auto &item) { return !is_used(item.value); });
    this->spheres.remove_if([](const auto &item) { return !is_used(item.value); });
    this->convex_hulls.remove_if([](const auto &item) { return !is_used(item.value); });
    this->compounds.remove_if([](const auto &item) { return !is_used(item.value); });
    this->decorated_shapes.remove_if([](const auto &item) { return !is_used(item.value); });
  }

  /* Lookup a cached shape without flagging it as used. */
  CachedShape *get_internal(const ShapeIDVariant &shape_id_variant)
  {
    if (const auto *shape_id = std::get_if<BoxID>(&shape_id_variant)) {
      return boxes.lookup_ptr(*shape_id);
    }
    if (const auto *shape_id = std::get_if<SphereID>(&shape_id_variant)) {
      return spheres.lookup_ptr(*shape_id);
    }
    if (const auto *shape_id = std::get_if<ConvexHullID>(&shape_id_variant)) {
      return convex_hulls.lookup_ptr(*shape_id);
    }
    if (const auto *shape_id = std::get_if<CompoundID>(&shape_id_variant)) {
      return compounds.lookup_ptr(*shape_id);
    }
    if (const auto *shape_id = std::get_if<DecoratedID>(&shape_id_variant)) {
      return decorated_shapes.lookup_ptr(*shape_id);
    }
    return nullptr;
  }

  JPH::ShapeSettings::ShapeResult get(const ShapeIDVariant &shape_id_variant)
  {
    CachedShape *shape = get_internal(shape_id_variant);
    if (!shape) {
      return {};
    }

    set_used(*shape);
    return *shape;
  }

  JPH::ShapeSettings::ShapeResult get_or_create_box(const CollisionShapeParams &params)
  {
    const std::optional<Bounds<float3>> bounds =
        params.geometry.compute_boundbox_without_instances(true);
    if (!bounds) {
      return {};
    }
    const float3 center = bounds->center() * params.scale;
    const float3 half_extent = 0.5f * bounds->size() * params.scale;
    JPH::ShapeSettings::ShapeResult shape = this->get_or_create_box(half_extent, params.density);
    if (math::is_zero(center, com_offset_threshold)) {
      return shape;
    }
    else {
      return this->get_or_create_rotated_translated(shape, center, math::Quaternion::identity());
    }
  }

  JPH::ShapeSettings::ShapeResult get_or_create_box(const float3 &half_extent, const float density)
  {
    const BoxID box_id{half_extent, density};
    CachedShape &cached_shape = this->boxes.lookup_or_add_cb(box_id, [&]() {
      JPH::BoxShapeSettings box_shape_settings{convert_vec3(half_extent)};
      box_shape_settings.mDensity = density;
      const float min_axis = std::min({half_extent.x, half_extent.y, half_extent.z});
      box_shape_settings.mConvexRadius = std ::min(0.5f * min_axis, default_convex_radius);
      return box_shape_settings.Create();
    });
    set_used(cached_shape);
    return cached_shape;
  }

  JPH::ShapeSettings::ShapeResult get_or_create_sphere(const CollisionShapeParams &params)
  {
    const std::optional<Bounds<float3>> bounds =
        params.geometry.compute_boundbox_without_instances(true);
    if (!bounds) {
      return {};
    }
    const float3 center = bounds->center() * params.scale;
    const float3 half_extent = 0.5f * bounds->size() * params.scale;
    const float radius = std::max({half_extent.x, half_extent.y, half_extent.z});
    JPH::ShapeSettings::ShapeResult shape = this->get_or_create_sphere(radius, params.density);
    if (math::is_zero(center, com_offset_threshold)) {
      return shape;
    }
    else {
      return this->get_or_create_rotated_translated(shape, center, math::Quaternion::identity());
    }
  }

  JPH::ShapeSettings::ShapeResult get_or_create_sphere(const float radius, const float density)
  {
    const SphereID sphere_id{radius, density};
    CachedShape &cached_shape = this->spheres.lookup_or_add_cb(sphere_id, [&]() {
      JPH::SphereShapeSettings sphere_shape_settings{radius};
      sphere_shape_settings.mDensity = density;
      return sphere_shape_settings.Create();
    });
    set_used(cached_shape);
    return cached_shape;
  }

  JPH::ShapeSettings::ShapeResult get_or_create_convex_hull(const CollisionShapeParams &params)
  {
    const auto shape_hash = geometry::GeometryShapeHash::from_geometry(params.geometry);
    const ConvexHullID convex_hull_id{shape_hash, params.scale, params.density};
    CachedShape &cached_shape = this->convex_hulls.lookup_or_add_cb(
        convex_hull_id, [&]() -> CachedShape {
          Vector<JPH::Vec3, 0, GuardedAlignedAllocator<>> points;
          if (const Mesh *mesh = params.geometry.get_mesh()) {
            points.reserve(points.size() + mesh->verts_num);
            const Span<float3> positions = mesh->vert_positions();
            for (const int i : positions.index_range()) {
              points.append(convert_vec3(positions[i] * params.scale));
            }
          }
          if (points.is_empty()) {
            return {};
          }
          JPH::ConvexHullShapeSettings hull_settings(
              points.data(), points.size(), default_convex_radius);
          hull_settings.mDensity = params.density;
          return hull_settings.Create();
        });
    set_used(cached_shape);
    return cached_shape;
  }

  /* TODO add a version with CollisionShapeParams which takes instances and shape types and looks
   * up sub shapes from the cache. */
  JPH::ShapeSettings::ShapeResult get_or_create_mutable_compound(
      const Span<JPH::ShapeSettings::ShapeResult> sub_shapes,
      const Span<float3> positions,
      const Span<math::Quaternion> rotations)
  {
    Array<CompoundID::SubShapeID> sub_shape_ids(sub_shapes.size());
    for (const int i : sub_shapes.index_range()) {
      sub_shape_ids[i] = {sub_shapes[i].Get(), positions[i], rotations[i]};
    }
    const CompoundID compound_id{std::move(sub_shape_ids)};
    CachedShape &cached_shape = this->compounds.lookup_or_add_cb(
        compound_id, [&]() -> CachedShape {
          JPH::MutableCompoundShapeSettings compound_settings;
          for (const int i : sub_shapes.index_range()) {
            compound_settings.AddShape(
                convert_vec3(positions[i]), convert_quat(rotations[i]), sub_shapes[i].Get());
          }
          return compound_settings.Create();
        });
    if (cached_shape.IsValid()) {
      for (const int i : sub_shapes.index_range()) {
        set_used(sub_shapes[i]);
      }
      set_used(cached_shape);
    }
    return cached_shape;
  }

  JPH::ShapeSettings::ShapeResult get_or_create_offset_center_of_mass(
      const JPH::ShapeSettings::ShapeResult &shape, const float3 &offset)
  {
    const DecoratedID decorated_id{shape.Get(), math::from_location<float4x4>(offset)};
    CachedShape &cached_shape = this->decorated_shapes.lookup_or_add_cb(decorated_id, [&]() {
      JPH::OffsetCenterOfMassShapeSettings offset_com_settings{convert_vec3(offset), shape.Get()};
      return offset_com_settings.Create();
    });
    if (cached_shape.IsValid()) {
      set_used(shape);
      set_used(cached_shape);
    }
    return cached_shape;
  }

  JPH::ShapeSettings::ShapeResult get_or_create_rotated_translated(
      const JPH::ShapeSettings::ShapeResult &shape,
      const float3 &translation,
      const math::Quaternion &rotation)
  {
    const DecoratedID decorated_id{shape.Get(),
                                   math::from_loc_rot<float4x4>(translation, rotation)};
    CachedShape &cached_shape = this->decorated_shapes.lookup_or_add_cb(decorated_id, [&]() {
      JPH::RotatedTranslatedShapeSettings loc_rot_settings{
          convert_vec3(translation), convert_quat(rotation), shape.Get()};
      return loc_rot_settings.Create();
    });
    if (cached_shape.IsValid()) {
      set_used(shape);
      set_used(cached_shape);
    }
    return cached_shape;
  }

  JPH::ShapeSettings::ShapeResult get_or_create_scaled(
      const JPH::ShapeSettings::ShapeResult &shape, const float3 &scale)
  {
    const DecoratedID decorated_id{shape.Get(), math::from_scale<float4x4>(scale)};
    CachedShape &cached_shape = this->decorated_shapes.lookup_or_add_cb(decorated_id, [&]() {
      JPH::ScaledShapeSettings scaled_settings{shape.Get(), convert_vec3(scale)};
      return scaled_settings.Create();
    });
    if (cached_shape.IsValid()) {
      set_used(shape);
      set_used(cached_shape);
    }
    return cached_shape;
  }
};

class JoltState : public xpbd_old::PhysicsState {
 public:
  JoltState()
  {
    jolt::ensure_initialization();

    /* Defaults taken from Jolt's Hello World example, may need to be tweaked/dynamic over
     * time.*/
    const int max_bodies = 65536;
    const int num_body_mutexes = 0;
    const int max_body_pairs = 65536;
    const int max_contact_constraints = 10240;
    this->system.Init(max_bodies,
                      num_body_mutexes,
                      max_body_pairs,
                      max_contact_constraints,
                      this->broad_phase_layer_interface,
                      this->object_vs_broad_phase_layer_filter,
                      this->object_layer_pair_filter);
    this->system.SetBodyActivationListener(&this->body_activation_listener);
    this->system.SetContactListener(&this->contact_listener);
    this->job_system.emplace(JPH::cMaxPhysicsJobs);
  }

  virtual ~JoltState() = default;

  int update_counter = 0;

  BroadPhaseLayerInterfaceImpl broad_phase_layer_interface;
  ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter;
  ObjectLayerPairFilterImpl object_layer_pair_filter;
  BodyActivationListenerImpl body_activation_listener;
  ContactListenerImpl contact_listener;
  JPH::PhysicsSystem system;
  /* TODO: Integrate with TBB. */
  std::optional<JPH::JobSystemSingleThreaded> job_system;

  Map<std::string, JoltRigidBodies> rigid_bodies_by_path;
  Map<std::string, JoltSoftBody> soft_bodies_by_path;
  CollisionShapeCache collision_shape_cache;
};

static void apply_motion_to_instance_transforms(const JoltRigidBodies &rigid_bodies,
                                                bke::Instances &instances)
{
  const int instances_num = instances.instances_num();
  const Span<int> instance_ids = instances.unique_ids();
  MutableSpan<float4x4> transforms = instances.transforms_for_write();

  for (const int instance_i : IndexRange(instances_num)) {
    const int instance_id = instance_ids[instance_i];
    const JoltRigidBody *rigid_body = rigid_bodies.bodies_by_id.lookup_ptr(instance_id);
    if (!rigid_body) {
      continue;
    }
    const JPH::Body &jolt_body = *rigid_body->body;
    if (jolt_body.GetMotionType() != JPH::EMotionType::Dynamic) {
      continue;
    }

    float4x4 &transform = transforms[instance_i];
    const JPH::Vec3 jolt_position = jolt_body.GetPosition();
    const JPH::Quat jolt_rotation = jolt_body.GetRotation();

    const float3 position = convert_vec3(jolt_position);
    const math::Quaternion rotation = convert_quat(jolt_rotation);

    /* Scale is not simulated to Jolt, so keep the scale of the original geometry. */
    const float3 scale = math::to_scale(transform);

    transform = math::from_loc_rot_scale<float4x4>(position, rotation, scale);
  }
}

static void apply_motion_to_soft_body_shape(const JoltSoftBody &soft_body, Mesh &mesh)
{
  BLI_assert(soft_body.body->IsSoftBody());
  const auto &motion_properties = *static_cast<const JPH::SoftBodyMotionProperties *>(
      soft_body.body->GetMotionProperties());
  const JPH::Array<JPH::SoftBodyVertex> &vertices = motion_properties.GetVertices();
  if (mesh.verts_num != vertices.size()) {
    return;
  }
  const JPH::Vec3 center = soft_body.body->GetPosition();
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const JPH::SoftBodyVertex &soft_body_vertex = vertices[i];
      positions[i] = convert_vec3(soft_body_vertex.mPosition + center);
    }
  });
  mesh.tag_positions_changed();
}

enum class CollisionShapeType {
  Box,
  Sphere,
  ConvexHull,
};

static std::optional<CollisionShapeType> parse_collision_shape_type(const int type)
{
  switch (type) {
    case 0:
      return CollisionShapeType::Box;
    case 1:
      return CollisionShapeType::Sphere;
    case 2:
      return CollisionShapeType::ConvexHull;
  }
  return std::nullopt;
}

static std::optional<JPH::EMotionType> parse_motion_type(const int type)
{
  switch (type) {
    case 0:
      return JPH::EMotionType::Dynamic;
    case 1:
      return JPH::EMotionType::Static;
    case 2:
      return JPH::EMotionType::Kinematic;
    default:
      return std::nullopt;
  }
}

static JPH::ShapeSettings::ShapeResult make_collision_shape(const CollisionShapeType type,
                                                            const CollisionShapeParams &params,
                                                            CollisionShapeCache &cache)
{
  switch (type) {
    case CollisionShapeType::Box: {
      return cache.get_or_create_box(params);
    }
    case CollisionShapeType::Sphere: {
      return cache.get_or_create_sphere(params);
    }
    case CollisionShapeType::ConvexHull: {
      return cache.get_or_create_convex_hull(params);
    }
  }
  BLI_assert_unreachable();
  return {};
}

static JoltRigidBodies rigid_bodies_from_instances(
    const bke::Instances &instances,
    const fn::Field<int> &collision_shape_type_field,
    const fn::Field<int> &motion_type_field,
    const fn::Field<float> &friction_field,
    const fn::Field<float> &bounciness_field,
    const fn::Field<float> &density_field,
    JoltRigidBodies *old_rigid_bodies,
    JPH::BodyInterface &body_interface,
    CollisionShapeCache &collision_shape_cache)
{
  const int instances_num = instances.instances_num();
  const int references_num = instances.references_num();
  const Span<int> instance_ids = instances.unique_ids();
  const Span<float4x4> transforms = instances.transforms();
  const Span<bke::InstanceReference> references = instances.references();
  const Span<int> handles = instances.reference_handles();

  bke::InstancesFieldContext field_context{instances};
  fn::FieldEvaluator field_evaluator{field_context, instances_num};
  field_evaluator.add(collision_shape_type_field);
  field_evaluator.add(motion_type_field);
  field_evaluator.add(friction_field);
  field_evaluator.add(bounciness_field);
  field_evaluator.add(density_field);
  field_evaluator.evaluate();
  const VArray<int> collision_shape_types = field_evaluator.get_evaluated<int>(0);
  const VArray<int> motion_types = field_evaluator.get_evaluated<int>(1);
  const VArray<float> frictions = field_evaluator.get_evaluated<float>(2);
  const VArray<float> bouncinesses = field_evaluator.get_evaluated<float>(3);
  const VArray<float> densities = field_evaluator.get_evaluated<float>(4);

  Array<bke::GeometrySet> reference_geometry_sets(references_num);
  for (const int i : references.index_range()) {
    const bke::InstanceReference &reference = references[i];
    bke::GeometrySet reference_geometry;
    reference.to_geometry_set(reference_geometry);
    reference_geometry_sets[i] = std::move(reference_geometry);
  }

  JoltRigidBodies rigid_bodies;
  for (const int instance_i : IndexRange(instances_num)) {
    const int reference_i = handles[instance_i];
    const int instance_id = instance_ids[instance_i];
    if (!reference_geometry_sets.index_range().contains(reference_i)) {
      continue;
    }
    const std::optional<CollisionShapeType> collision_shape_type = parse_collision_shape_type(
        collision_shape_types[instance_i]);
    if (!collision_shape_type) {
      continue;
    }
    const std::optional<JPH::EMotionType> motion_type = parse_motion_type(
        motion_types[instance_i]);
    if (!motion_type) {
      continue;
    }
    const bke::GeometrySet &reference_geometry = reference_geometry_sets[reference_i];
    const std::optional<Bounds<float3>> bounds =
        reference_geometry.compute_boundbox_without_instances(true);
    if (!bounds) {
      continue;
    }
    const float4x4 &instance_transform = transforms[instance_i];
    float3 instance_position;
    math::Quaternion instance_rotation;
    float3 instance_scale;
    math::to_loc_rot_scale_safe<true>(
        instance_transform, instance_position, instance_rotation, instance_scale);

    float density = densities[instance_i];
    if (density <= 0.0f) {
      density = 1.0f;
    }

    const CollisionShapeParams collision_shape_params{reference_geometry, instance_scale, density};
    JPH::ShapeSettings::ShapeResult collision_shape = make_collision_shape(
        *collision_shape_type, collision_shape_params, collision_shape_cache);
    if (!collision_shape.IsValid()) {
      continue;
    }

    const float friction = frictions[instance_i];
    const float bounciness = bouncinesses[instance_i];

    std::optional<JoltRigidBody> rigid_body;
    if (old_rigid_bodies) {
      if (std::optional<JoltRigidBody> old_rigid_body = old_rigid_bodies->bodies_by_id.pop_try(
              instance_id))
      {
        BLI_assert(int(old_rigid_body->body->GetUserData()) == instance_id);
        rigid_body = old_rigid_body;
      }
    }
    if (!rigid_body) {
      JPH::BodyCreationSettings jolt_body_settings{collision_shape.Get(),
                                                   convert_vec3(instance_position),
                                                   convert_quat(instance_rotation),
                                                   *motion_type,
                                                   *motion_type == JPH::EMotionType::Dynamic ?
                                                       ObjectLayers::moving :
                                                       ObjectLayers::non_moving};
      jolt_body_settings.mUserData = uint64_t(instance_id);

      JPH::Body *jolt_body = body_interface.CreateBody(jolt_body_settings);
      if (!jolt_body) {
        continue;
      }
      body_interface.AddBody(jolt_body->GetID(), JPH::EActivation::Activate);
      rigid_body = JoltRigidBody{jolt_body};
    }

    body_interface.SetShape(
        rigid_body->body->GetID(), collision_shape.Get(), true, JPH::EActivation::Activate);
    rigid_body->body->SetFriction(friction);
    rigid_body->body->SetRestitution(bounciness);

    if (motion_type == JPH::EMotionType::Kinematic) {
      body_interface.SetPositionAndRotationWhenChanged(rigid_body->body->GetID(),
                                                       convert_vec3(instance_position),
                                                       convert_quat(instance_rotation),
                                                       JPH::EActivation::Activate);
    }

    rigid_bodies.bodies_by_id.add(instance_id, std::move(*rigid_body));
  }

  return rigid_bodies;
}

static float compute_soft_body_compliance(float stretch_stiffness)
{
  stretch_stiffness = std::max(0.0f, stretch_stiffness);
  return stretch_stiffness == 0.0f ? 1.0f : 1.0f / stretch_stiffness;
}

static float compute_soft_body_shear_compliance(float bend_stiffness)
{
  bend_stiffness = std::max(0.0f, bend_stiffness);
  return bend_stiffness == 0.0f ? 1.0f : 1.0f / bend_stiffness;
}

static float compute_soft_body_bend_compliance(float bend_stiffness)
{
  bend_stiffness = std::max(0.0f, bend_stiffness);
  return bend_stiffness == 0.0f ? 1.0f : 1.0f / bend_stiffness;
}

static JoltSoftBody soft_body_from_mesh(const Mesh &mesh,
                                        const fn::Field<float> &stretch_stiffness_field,
                                        const fn::Field<float> &bend_stiffness_field,
                                        JoltSoftBody *old_soft_body,
                                        JPH::BodyInterface &body_interface)
{
  JoltSoftBody soft_body;
  if (old_soft_body) {
    const auto &motion_properties = *static_cast<const JPH::SoftBodyMotionProperties *>(
        old_soft_body->body->GetMotionProperties());
    if (motion_properties.GetVertices().size() == mesh.verts_num) {
      soft_body.body = old_soft_body->body;
      old_soft_body->body = nullptr;
    }
  }
  if (!soft_body.body) {
    bke::MeshFieldContext field_context{mesh, bke::AttrDomain::Point};
    fn::FieldEvaluator field_evaluator{field_context, mesh.verts_num};
    field_evaluator.add(stretch_stiffness_field);
    field_evaluator.add(bend_stiffness_field);
    field_evaluator.evaluate();
    const VArray<float> stretch_stiffnesses = field_evaluator.get_evaluated<float>(0);
    const VArray<float> bend_stiffnesses = field_evaluator.get_evaluated<float>(1);

    JPH::Ref<JPH::SoftBodySharedSettings> shared_settings = new JPH::SoftBodySharedSettings();
    const Span<float3> positions = mesh.vert_positions();

    for (int i : positions.index_range()) {
      const float3 &position = positions[i];
      JPH::SoftBodySharedSettings::Vertex vertex;
      convert_vec3(position).StoreFloat3(&vertex.mPosition);
      shared_settings->mVertices.push_back(vertex);
    }

    const Span<int3> tris = mesh.corner_tris();
    const Span<int> corner_verts = mesh.corner_verts();
    for (int i : tris.index_range()) {
      JPH::SoftBodySharedSettings::Face face;
      const int3 &tri = tris[i];
      face.mVertex[0] = corner_verts[tri.x];
      face.mVertex[1] = corner_verts[tri.y];
      face.mVertex[2] = corner_verts[tri.z];
      shared_settings->AddFace(face);
    }

    Vector<JPH::SoftBodySharedSettings::VertexAttributes> vertex_attributes_vec;
    if (bend_stiffnesses.is_single() && stretch_stiffnesses.is_single()) {
      JPH::SoftBodySharedSettings::VertexAttributes attributes;
      attributes.mCompliance = compute_soft_body_compliance(
          stretch_stiffnesses.get_internal_single());
      attributes.mShearCompliance = compute_soft_body_shear_compliance(
          bend_stiffnesses.get_internal_single());
      attributes.mBendCompliance = compute_soft_body_bend_compliance(
          bend_stiffnesses.get_internal_single());
      vertex_attributes_vec.append(attributes);
    }
    else {
      vertex_attributes_vec.resize(positions.size());
      threading::parallel_for(IndexRange(positions.size()), 1024, [&](const IndexRange range) {
        for (const int i : range) {
          JPH::SoftBodySharedSettings::VertexAttributes &attributes = vertex_attributes_vec[i];
          attributes.mCompliance = compute_soft_body_compliance(stretch_stiffnesses[i]);
          attributes.mShearCompliance = compute_soft_body_shear_compliance(bend_stiffnesses[i]);
          attributes.mBendCompliance = compute_soft_body_bend_compliance(bend_stiffnesses[i]);
        }
      });
    }

    shared_settings->CreateConstraints(vertex_attributes_vec.data(),
                                       vertex_attributes_vec.size(),
                                       JPH::SoftBodySharedSettings::EBendType::Dihedral);
    shared_settings->Optimize();

    JPH::SoftBodyCreationSettings soft_body_creation_settings(
        shared_settings, JPH::Vec3(0, 0, 0), JPH::Quat::sIdentity(), ObjectLayers::moving);
    soft_body.body = body_interface.CreateSoftBody(soft_body_creation_settings);
    if (soft_body.body) {
      body_interface.AddBody(soft_body.body->GetID(), JPH::EActivation::Activate);
    }
  }

  return soft_body;
}

static void update_gravity(JoltState &state)
{
  /* TODO GeoNodeExecParams are only used for error messages in case of redundant gravity behavior
   * here. This should be checked when parsing behaviors before it goes into the solver! */
  // if (world.gravities.size() >= 2) {
  //   params.error_message_add(NodeWarningType::Warning, "There can't be multiple gravities");
  //   state.system.SetGravity(JPH::Vec3::sZero());
  //   return;
  // }
  // if (world.gravities.size() == 1) {
  //   const float3 gravity = world.gravities[0].gravity;
  //   state.system.SetGravity(convert_vec3(gravity));
  //   return;
  // }

  const JPH::Vec3 default_gravity(0.0f, 0.0f, -9.81f);
  state.system.SetGravity(default_gravity);
}

struct ContactPoints {
  /* Jolt body ID of colliding body 2. */
  Vector<uint32_t> body_id2;
  /* Instance ID of colliding body 2. */
  Vector<int> instance_id2;
  /* Contact point on the surface in local space of body 1. */
  Vector<float3> contact_point1;
  /* Contact point on the surface in local space of body 2. */
  Vector<float3> contact_point2;
  /* Direction in world space to move body 2 out of collision (not normalized). */
  Vector<float3> penetration_axis;
  /* Distance to move along the penetration axis to separate the two bodies. */
  Vector<float> penetration_depth;
  /* Face ID on body 1. */
  Vector<int> subshape_id1;
  /* Face ID on body 2. */
  Vector<int> subshape_id2;

  ContactPoints(const int initial_capacity = 0)
  {
    this->body_id2.reserve(initial_capacity);
    this->instance_id2.reserve(initial_capacity);
    this->contact_point1.reserve(initial_capacity);
    this->contact_point2.reserve(initial_capacity);
    this->penetration_axis.reserve(initial_capacity);
    this->penetration_depth.reserve(initial_capacity);
    this->subshape_id1.reserve(initial_capacity);
    this->subshape_id2.reserve(initial_capacity);
  }
};

struct ContactPointCollector : public JPH::CollideShapeCollector {
  const JPH::BodyInterface &body_interface_;
  float3 body1_location_;
  math::Quaternion inv_body1_rotation_;
  ContactPoints &contacts_;

  ContactPointCollector(const JPH::BodyInterface &body_interface,
                        const float3 &body1_location,
                        const math::Quaternion &body1_rotation,
                        ContactPoints &contacts)
      : body_interface_(body_interface),
        body1_location_(body1_location),
        inv_body1_rotation_(math::invert_normalized(body1_rotation)),
        contacts_(contacts)
  {
  }

  void AddHit(const ResultType &result) override
  {
    JPH::RVec3 jolt_body2_location;
    JPH::Quat jolt_body2_rotation;
    body_interface_.GetPositionAndRotation(
        result.mBodyID2, jolt_body2_location, jolt_body2_rotation);
    const math::Quaternion inv_body2_rotation = math::invert_normalized(
        convert_quat(jolt_body2_rotation));
    const float3 body2_location = convert_vec3(jolt_body2_location);

    contacts_.body_id2.append(result.mBodyID2.GetIndexAndSequenceNumber());
    contacts_.instance_id2.append(body_interface_.GetUserData(result.mBodyID2));

    /* Contact points are given in world space relative to body1 origin.
     * Output points are in local space of body 1 and body 2 respectively. */
    contacts_.contact_point1.append(
        math::transform_point(inv_body1_rotation_, convert_vec3(result.mContactPointOn1)));
    contacts_.contact_point2.append(math::transform_point(inv_body2_rotation,
                                                          convert_vec3(result.mContactPointOn2) +
                                                              body1_location_ - body2_location));

    contacts_.penetration_axis.append(convert_vec3(result.mPenetrationAxis));
    contacts_.penetration_depth.append(result.mPenetrationDepth);
    contacts_.subshape_id1.append(int(result.mSubShapeID1.GetValue()));
    contacts_.subshape_id2.append(int(result.mSubShapeID2.GetValue()));
  }
};

static void collide_shape(const JoltState &state,
                          const JPH::Shape &shape,
                          const float4x4 &transform,
                          const float speculative_contact_distance,
                          ContactPoints &r_contacts)
{
  /* No lock required, this happens outside the physics update. */
  const JPH::NarrowPhaseQuery &query = state.system.GetNarrowPhaseQueryNoLock();

  float3 location;
  math::Quaternion rotation;
  float3 scale;
  math::to_loc_rot_scale(transform, location, rotation, scale);
  const JPH::RMat44 center_of_mass_transform = JPH::RMat44::sRotationTranslation(
      convert_quat(rotation), convert_vec3(location));
  /* TODO decide if anything here needs to be configurable. */
  JPH::CollideShapeSettings settings;
  /* Detect contacts before penetration happens. */
  settings.mMaxSeparationDistance = speculative_contact_distance;

  /* Picking the reference location of the contact can improve accuracy. In most cases the shape
   * origin is a good choice. */
  const float3 base_offset = location;

  ContactPointCollector collector(
      state.system.GetBodyInterfaceNoLock(), location, rotation, r_contacts);

  const JPH::BroadPhaseLayerFilter broad_phase_layer_filter = {};
  const JPH::ObjectLayerFilter object_layer_filter = {};
  /* TODO The body filter should be used to restrict contacts based on a bundle path! This may
   * require building a reverse map or set of valid instance IDs to identify valid contacts in the
   * collector. */
  const JPH::BodyFilter body_filter = {};
  const JPH::ShapeFilter shape_filter = {};

  /* TODO Can also be done using a shape cast, i.e. shape moving along a ray. This could
   * provide more complete collision detection in case of fast movement but is more expensive.
   */
  query.CollideShape(&shape,
                     convert_vec3(scale),
                     center_of_mass_transform,
                     settings,
                     convert_vec3(base_offset),
                     collector,
                     broad_phase_layer_filter,
                     object_layer_filter,
                     body_filter,
                     shape_filter);
}

}  // namespace blender::geometry::jolt_physics

namespace blender::geometry::xpbd_old {

using nodes::nested_bundle_path_is_selected;

constexpr StringRefNull prev_position_name = ".prev_position";
constexpr StringRefNull prev_rotation_name = ".prev_rotation";

PhysicsState::~PhysicsState() {}

PhysicsState *PhysicsState::create()
{
  return MEM_new<jolt_physics::JoltState>(__func__);
}

static bke::GeometrySet apply_rigid_body_simulation(const RigidBodyInstances &rigid_body_instances,
                                                    const jolt_physics::JoltState &state)
{
  bke::GeometrySet geometry = rigid_body_instances.instances_geometry;
  bke::Instances *instances = geometry.get_instances_for_write();
  if (!instances) {
    return geometry;
  }

  const jolt_physics::JoltRigidBodies *rigid_bodies = state.rigid_bodies_by_path.lookup_ptr(
      rigid_body_instances.self_path);
  if (!rigid_bodies) {
    return geometry;
  }

  jolt_physics::apply_motion_to_instance_transforms(*rigid_bodies, *instances);
  return geometry;
}

static bke::GeometrySet apply_soft_body_simulation(const SoftBodyMesh &soft_body_mesh,
                                                   const jolt_physics::JoltState &state)
{
  bke::GeometrySet geometry = soft_body_mesh.mesh_geometry;
  Mesh *mesh = geometry.get_mesh_for_write();
  if (!mesh) {
    return geometry;
  }

  const jolt_physics::JoltSoftBody *soft_body = state.soft_bodies_by_path.lookup_ptr(
      soft_body_mesh.self_path);
  if (!soft_body) {
    return geometry;
  }

  jolt_physics::apply_motion_to_soft_body_shape(*soft_body, *mesh);
  return geometry;
}

static void update_jolt_state_from_behaviors(jolt_physics::JoltState &state,
                                             const Behaviors &behaviors)
{
  state.collision_shape_cache.reset_used();

  Array<bke::GeometrySet> applied_rigid_bodies(behaviors.rigid_body_instances.size());
  for (const int i : behaviors.rigid_body_instances.index_range()) {
    applied_rigid_bodies[i] = apply_rigid_body_simulation(behaviors.rigid_body_instances[i],
                                                          state);
  }
  Array<bke::GeometrySet> applied_soft_bodies(behaviors.soft_body_meshes.size());
  for (const int i : behaviors.soft_body_meshes.index_range()) {
    applied_soft_bodies[i] = apply_soft_body_simulation(behaviors.soft_body_meshes[i], state);
  }

  JPH::BodyInterface &body_interface = state.system.GetBodyInterfaceNoLock();

  Map<std::string, jolt_physics::JoltRigidBodies> new_rigid_bodies_by_path;
  for (const int i : behaviors.rigid_body_instances.index_range()) {
    const bke::GeometrySet &applied_rigid_body = applied_rigid_bodies[i];
    const bke::Instances *instances = applied_rigid_body.get_instances();
    if (!instances) {
      continue;
    }
    const RigidBodyInstances &rigid_body_instances = behaviors.rigid_body_instances[i];
    jolt_physics::JoltRigidBodies *old_rigid_bodies = state.rigid_bodies_by_path.lookup_ptr(
        rigid_body_instances.self_path);
    jolt_physics::JoltRigidBodies rigid_bodies = jolt_physics::rigid_bodies_from_instances(
        *instances,
        rigid_body_instances.collision_shape_type,
        rigid_body_instances.motion_type,
        rigid_body_instances.friction,
        rigid_body_instances.bounciness,
        rigid_body_instances.density,
        old_rigid_bodies,
        body_interface,
        state.collision_shape_cache);
    if (!rigid_bodies.bodies_by_id.is_empty()) {
      new_rigid_bodies_by_path.add(rigid_body_instances.self_path, std::move(rigid_bodies));
    }
  }
  Map<std::string, jolt_physics::JoltSoftBody> new_soft_bodies_by_path;
  for (const int i : behaviors.soft_body_meshes.index_range()) {
    const bke::GeometrySet &applied_soft_body = applied_soft_bodies[i];
    const Mesh *mesh = applied_soft_body.get_mesh();
    if (!mesh) {
      continue;
    }
    const SoftBodyMesh &soft_body_mesh = behaviors.soft_body_meshes[i];
    jolt_physics::JoltSoftBody *old_soft_body = state.soft_bodies_by_path.lookup_ptr(
        soft_body_mesh.self_path);
    jolt_physics::JoltSoftBody soft_body = jolt_physics::soft_body_from_mesh(
        *mesh,
        soft_body_mesh.stretch_stiffness,
        soft_body_mesh.bend_stiffness,
        old_soft_body,
        body_interface);
    if (soft_body.body) {
      new_soft_bodies_by_path.add(soft_body_mesh.self_path, std::move(soft_body));
    }
  }

  /* Remove old bodies. */
  Vector<JPH::BodyID> bodies_to_remove;
  for (jolt_physics::JoltRigidBodies &rigid_bodies : state.rigid_bodies_by_path.values()) {
    for (jolt_physics::JoltRigidBody &body : rigid_bodies.bodies_by_id.values()) {
      bodies_to_remove.append(body.body->GetID());
    }
  }
  for (jolt_physics::JoltSoftBody &soft_body : state.soft_bodies_by_path.values()) {
    if (soft_body.body) {
      bodies_to_remove.append(soft_body.body->GetID());
    }
  }
  body_interface.RemoveBodies(bodies_to_remove.data(), bodies_to_remove.size());
  body_interface.DestroyBodies(bodies_to_remove.data(), bodies_to_remove.size());

  state.rigid_bodies_by_path = std::move(new_rigid_bodies_by_path);
  state.soft_bodies_by_path = std::move(new_soft_bodies_by_path);
  state.collision_shape_cache.remove_unused();

  jolt_physics::update_gravity(state);
  // TODO
  // apply_forces(state, world, applied_rigid_bodies);
}

struct ContactPointAttributeNames {
  static const std::string body_id2;
  static const std::string instance_id2;
  static const std::string contact_point1;
  static const std::string contact_point2;
  static const std::string penetration_axis;
  static const std::string penetration_depth;
  static const std::string subshape_id1;
  static const std::string subshape_id2;
};

const std::string ContactPointAttributeNames::body_id2 = "body_id2";
const std::string ContactPointAttributeNames::instance_id2 = "instance_id2";
const std::string ContactPointAttributeNames::contact_point1 = "position";
const std::string ContactPointAttributeNames::contact_point2 = "contact_point2";
const std::string ContactPointAttributeNames::penetration_axis = "penetration_axis";
const std::string ContactPointAttributeNames::penetration_depth = "penetration_depth";
const std::string ContactPointAttributeNames::subshape_id1 = "subshape_id1";
const std::string ContactPointAttributeNames::subshape_id2 = "subshape_id2";

static bke::GeometrySet pointcloud_from_contacts(jolt_physics::ContactPoints &&contacts)
{
  PointCloud *pointcloud = BKE_pointcloud_new_nomain(contacts.body_id2.size());
  bke::MutableAttributeAccessor attributes = pointcloud->attributes_for_write();

  attributes.add<int>(
      ContactPointAttributeNames::body_id2,
      bke::AttrDomain::Point,
      bke::AttributeInitVArray(VArray<uint32_t>::from_container(std::move(contacts.body_id2))));
  attributes.add<int>(
      ContactPointAttributeNames::instance_id2,
      bke::AttrDomain::Point,
      bke::AttributeInitVArray(VArray<int>::from_container(std::move(contacts.instance_id2))));
  /* Contact point 1 is stored in positions. Contact point 2 is a separate attribute. */
  array_utils::copy(contacts.contact_point1.as_span(), pointcloud->positions_for_write());
  attributes.add<float3>(ContactPointAttributeNames::contact_point2,
                         bke::AttrDomain::Point,
                         bke::AttributeInitVArray(
                             VArray<float3>::from_container(std::move(contacts.contact_point2))));
  attributes.add<float3>(ContactPointAttributeNames::penetration_axis,
                         bke::AttrDomain::Point,
                         bke::AttributeInitVArray(VArray<float3>::from_container(
                             std::move(contacts.penetration_axis))));
  attributes.add<float>(ContactPointAttributeNames::penetration_depth,
                        bke::AttrDomain::Point,
                        bke::AttributeInitVArray(
                            VArray<float>::from_container(std::move(contacts.penetration_depth))));
  attributes.add<int>(
      ContactPointAttributeNames::subshape_id1,
      bke::AttrDomain::Point,
      bke::AttributeInitVArray(VArray<int>::from_container(std::move(contacts.subshape_id1))));
  attributes.add<int>(
      ContactPointAttributeNames::subshape_id2,
      bke::AttrDomain::Point,
      bke::AttributeInitVArray(VArray<int>::from_container(std::move(contacts.subshape_id2))));
  pointcloud->tag_positions_changed();

  return bke::GeometrySet::from_pointcloud(pointcloud);
}

SimGeometry::SimGeometry(SimGeometrySet &src, GeometryVariant data) : data(data), src(src) {}

std::optional<bke::AttributeAccessor> SimGeometry::attributes() const
{
  if (const Mesh *const *mesh = std::get_if<Mesh *>(&data)) {
    return (*mesh)->attributes();
  }
  if (const PointCloud *const *pointcloud = std::get_if<PointCloud *>(&data)) {
    return (*pointcloud)->attributes();
  }
  if (const Curves *const *curves = std::get_if<Curves *>(&data)) {
    return (*curves)->geometry.wrap().attributes();
  }
  return std::nullopt;
}

std::optional<bke::MutableAttributeAccessor> SimGeometry::attributes_for_write()
{
  if (Mesh **mesh = std::get_if<Mesh *>(&data)) {
    return (*mesh)->attributes_for_write();
  }
  if (PointCloud **pointcloud = std::get_if<PointCloud *>(&data)) {
    return (*pointcloud)->attributes_for_write();
  }
  if (Curves **curves = std::get_if<Curves *>(&data)) {
    return (*curves)->geometry.wrap().attributes_for_write();
  }
  return std::nullopt;
}

int SimGeometry::points_num() const
{
  if (std::optional<bke::AttributeAccessor> attributes = this->attributes()) {
    return attributes->domain_size(bke::AttrDomain::Point);
  }
  return 0;
}

int SimGeometry::set_point_field_context(std::optional<bke::GeometryFieldContext> &r_context) const
{
  if (const Mesh *const *mesh = std::get_if<Mesh *>(&data)) {
    r_context.emplace(**mesh, bke::AttrDomain::Point);
    return (*mesh)->verts_num;
  }
  if (const PointCloud *const *pointcloud = std::get_if<PointCloud *>(&data)) {
    r_context.emplace(**pointcloud, bke::AttrDomain::Point);
    return (*pointcloud)->totpoint;
  }
  if (const Curves *const *curves = std::get_if<Curves *>(&data)) {
    r_context.emplace(**curves, bke::AttrDomain::Point);
    return (*curves)->geometry.point_num;
  }
  return 0;
}

nodes::Bundle &SimGeometrySet::extra_for_write()
{
  /* Ensure the caller locked it already. */
  BLI_assert(!this->extra_mutex.try_lock());
  if (!this->extra) {
    this->extra = nodes::Bundle::create();
  }
  else if (!this->extra->is_mutable()) {
    this->extra = this->extra->copy();
    this->extra->tag_ensured_mutable();
  }
  return const_cast<nodes::Bundle &>(*this->extra);
}

template<typename T> void SimGeometrySet::set_extra(const StringRef key, T value)
{
  std::scoped_lock lock(this->extra_mutex);
  nodes::Bundle &extra = this->extra_for_write();
  extra.add_override<std::decay_t<T>>(key, std::move(value));
}

void SimGeometrySet::remove_extra(const StringRef key)
{
  std::scoped_lock lock(this->extra_mutex);
  nodes::Bundle &extra = this->extra_for_write();
  extra.remove(key);
}

template<typename T> std::optional<T> SimGeometrySet::get_extra(const StringRef key) const
{
  std::scoped_lock lock(this->extra_mutex);
  if (!this->extra) {
    return std::nullopt;
  }
  return this->extra->lookup<T>(key);
}

class ConstraintContext {
 public:
  PhysicsState *physics_state;
};

void ConstraintSet::ensure_init(const ConstraintContext & /*context*/,
                                MutableSpan<SimGeometry> /*sim_geometries*/)
{
}

void ConstraintSet::solve(ConstraintSetSolveParams & /*params*/) {}

void ConstraintSet::post_solve_apply(MutableSpan<SimGeometry> /*sim_geometries*/,
                                     const PhysicsState * /*physics_state*/)
{
}

LocalConstraintCorrections &ConstraintCorrections::local()
{
  return local_corrections_;
}

LocalConstraintCorrections::LocalConstraintCorrections(ConstraintCorrections &corrections)
    : corrections_(corrections)
{
}

ConstraintCorrections::ConstraintCorrections(MutableSpan<SimGeometry> sim_geometries)
    : sim_geometries_(sim_geometries), local_corrections_(*this)
{
  corrections_.reinitialize(sim_geometries.size());
  for (const int geometry_i : sim_geometries.index_range()) {
    const int points_num = sim_geometries[geometry_i].points_num();
    corrections_[geometry_i].position_corrections.reinitialize(points_num);
    corrections_[geometry_i].rotation_corrections.reinitialize(points_num);
  }
}

void ConstraintCorrections::apply()
{
  threading::parallel_for(
      sim_geometries_.index_range(), 1, [&](const IndexRange sim_geometries_range) {
        for (const int geometry_i : sim_geometries_range) {
          SimGeometry &sim_geometry = sim_geometries_[geometry_i];
          std::optional<bke::MutableAttributeAccessor> attributes =
              sim_geometry.attributes_for_write();
          if (!attributes) {
            continue;
          }
          SimGeometryCorrections &corrections = corrections_[geometry_i];
          bke::SpanAttributeWriter<float3> positions = attributes->lookup_for_write_span<float3>(
              "position");
          /* Existence of rotation attribute means rotation constraints are valid on the
           * geometry. */
          bke::SpanAttributeWriter<math::Quaternion> rotations =
              attributes->lookup_for_write_span<math::Quaternion>(
                  sim_geometry.src.rotation_attribute);

          threading::parallel_for(
              positions.span.index_range(), 512, [&](const IndexRange points_range) {
                const float quantize_scale = sim_geometry.quantize_scale;
                for (const int point_i : points_range) {
                  const PositionCorrection &correction = corrections.position_corrections[point_i];
                  if (correction.num_corrections == 0) {
                    continue;
                  }
                  const int3 offset_quantized = correction.offset;
                  const float relaxation_factor = 1.3f;
                  const float factor = relaxation_factor /
                                       (quantize_scale * float(correction.num_corrections));
                  const float3 offset = float3(offset_quantized) * factor;
                  float3 &position = positions.span[point_i];
                  position += offset;
                }
              });

          if (rotations) {
            /* TODO This is a simple linear average for quaternions, which works well if the
             * rotations are close to each other. For larger differences a better approach is the
             * "maximum likelihood method", see Markley et al. "Averaging Quaternions"
             * (https://www.acsu.buffalo.edu/%7Ejohnc/ave_quat07.pdf) */
            threading::parallel_for(
                rotations.span.index_range(), 512, [&](const IndexRange points_range) {
                  const float quantize_scale = sim_geometry.quantize_scale;
                  for (const int point_i : points_range) {
                    const RotationCorrection &correction =
                        corrections.rotation_corrections[point_i];
                    if (correction.num_corrections == 0) {
                      continue;
                    }
                    const int4 offset_quantized = correction.offset;
                    const float factor = 1.0f /
                                         (quantize_scale * float(correction.num_corrections));
                    const float4 offset = float4(offset_quantized) * factor;
                    math::Quaternion &rotation = rotations.span[point_i];
                    rotation = math::normalize(math::Quaternion(float4(rotation) + offset));
                  }
                });
          }

          positions.finish();
          rotations.finish();
        }
      });
}

static void solve_distance_constraint(const int geometry0,
                                      const int geometry1,
                                      const int i0,
                                      const int i1,
                                      const float3 &p0,
                                      const float3 &p1,
                                      const float weight_pos0,
                                      const float weight_pos1,
                                      const float compliance_term,
                                      const float rest_distance,
                                      LocalConstraintCorrections &local_corrections)
{
  const float3 p_diff = p1 - p0;
  float length;
  const float3 normalized_dir = math::normalize_and_get_length(p_diff, length);

  float length_diff = length - rest_distance;
  const float lambda = length_diff / (weight_pos0 + weight_pos1 + compliance_term);

  const float3 correction0 = lambda * weight_pos0 * normalized_dir;
  const float3 correction1 = -lambda * weight_pos1 * normalized_dir;
  local_corrections.add_position_correction(geometry0, i0, correction0);
  local_corrections.add_position_correction(geometry1, i1, correction1);
}

/* Extended stretch/shear constraint that ensures segment length as well as aligning the rotation
 * with the direction of the segment. */
static void solve_distance_rotation_constraint(const int geometry_p0,
                                               const int geometry_p1,
                                               const int geometry_r0,
                                               const int p_i0,
                                               const int p_i1,
                                               const int r_i0,
                                               const float3 &p0,
                                               const float3 &p1,
                                               const math::Quaternion &r0,
                                               const float weight_pos0,
                                               const float weight_pos1,
                                               const float weight_rot0,
                                               const float3 &lambda_prev,
                                               const float compliance_term,
                                               const float rest_distance,
                                               LocalConstraintCorrections &local_corrections)
{
  BLI_assert(rest_distance > 0.0f);

  const float weight_sum = weight_pos0 + weight_pos1 +
                           4.0f * weight_rot0 * rest_distance * rest_distance;

  const float3 p_diff = p1 - p0;
  const float3 forward = math::transform_point(r0, float3(0, 0, 1));
  const float3 residual = p_diff / rest_distance - forward;

  const float3 lambda = (residual - compliance_term * lambda_prev) /
                        (weight_sum + compliance_term);

  const float3 correction_p0 = lambda * weight_pos0 * rest_distance;
  const float3 correction_p1 = -lambda * weight_pos1 * rest_distance;
  const math::Quaternion correction_r0 = math::Quaternion(0.0f,
                                                          lambda * weight_rot0 * rest_distance *
                                                              rest_distance) *
                                         r0 * math::Quaternion(0, 0, 0, -1);
  local_corrections.add_position_correction(geometry_p0, p_i0, correction_p0);
  local_corrections.add_position_correction(geometry_p1, p_i1, correction_p1);
  local_corrections.add_rotation_correction(geometry_r0, r_i0, correction_r0);
}

static void solve_bending_constraint(const int geometry_r0,
                                     const int geometry_r1,
                                     const int r_i0,
                                     const int r_i1,
                                     const math::Quaternion &r0,
                                     const math::Quaternion &r1,
                                     const float weight_rot0,
                                     const float weight_rot1,
                                     const float4 &lambda_prev,
                                     const float compliance_term,
                                     const math::Quaternion &rest_shape,
                                     LocalConstraintCorrections &local_corrections)
{
  const float weight_sum = weight_rot0 + weight_rot1;

  const math::Quaternion shape = math::invert_normalized(r0) * r1;

  /* TODO In "Position and Orientation Based Cosserat Rods" (Kugelstadt, Schoemer) the W component
   * of the Darboux vector is ignored. In "Sag-Free Initialization for Strand-Based Hybrid Hair
   * Simulation" (Hsu et al.) it is included.
   * For now stick to the float3 version. */
  const float4 rest_shapef = float4(0.0f, rest_shape.imaginary_part());
  const float4 shapef = float4(0.0f, shape.imaginary_part());

  const float4 residual_neg = shapef - rest_shapef;
  const float4 residual_pos = shapef + rest_shapef;
  const float4 residual = math::length_squared(residual_neg) < math::length_squared(residual_pos) ?
                              residual_neg :
                              residual_pos;

  const float4 lambda = (residual - compliance_term * lambda_prev) /
                        (weight_sum + compliance_term);

  const math::Quaternion correction_r0 = r1 * math::Quaternion(lambda * weight_rot0);
  const math::Quaternion correction_r1 = r0 * math::Quaternion(-lambda * weight_rot1);
  local_corrections.add_rotation_correction(geometry_r0, r_i0, correction_r0);
  local_corrections.add_rotation_correction(geometry_r1, r_i1, correction_r1);
}

class EdgeLengthConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  std::string rest_length_attribute_;
  float compliance_;

 public:
  EdgeLengthConstraintSet(std::string self_path,
                          std::string filter,
                          std::string rest_length_attribute,
                          const float compliance)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        rest_length_attribute_(std::move(rest_length_attribute)),
        compliance_(compliance)
  {
  }

  void ensure_init(const ConstraintContext & /*context*/,
                   MutableSpan<SimGeometry> sim_geometries) override
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      Mesh **mesh_ptr = std::get_if<Mesh *>(&sim_geometry.data);
      if (!mesh_ptr) {
        continue;
      }
      Mesh &mesh = **mesh_ptr;
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      if (attributes.contains(rest_length_attribute_)) {
        continue;
      }

      float *rest_lengths = MEM_malloc_arrayN<float>(mesh.edges_num, __func__);
      const Span<float3> positions = mesh.vert_positions();
      const Span<int2> edges = mesh.edges();
      threading::parallel_for(IndexRange(mesh.edges_num), 512, [&](const IndexRange range) {
        for (const int i : range) {
          const int2 edge = edges[i];
          const float length = math::distance(positions[edge[0]], positions[edge[1]]);
          rest_lengths[i] = length;
        }
      });
      attributes.add<float>(rest_length_attribute_,
                            bke::AttrDomain::Edge,
                            bke::AttributeInitMoveArray{rest_lengths});
    }
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    float compliance_term = 0.0f;
    if (params.delta_time > 0.0f) {
      compliance_term = compliance_ / pow2f(params.delta_time);
    }
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      const Mesh *const *mesh_ptr = std::get_if<Mesh *>(&sim_geometry.data);
      if (!mesh_ptr) {
        continue;
      }
      const Mesh &mesh = **mesh_ptr;
      const bke::AttributeAccessor attributes = mesh.attributes();
      const Span<int2> edges = mesh.edges();
      const Span<float3> positions = mesh.vert_positions();
      const bke::AttributeReader<float> rest_lengths = attributes.lookup<float>(
          rest_length_attribute_, bke::AttrDomain::Edge);
      if (!rest_lengths) {
        continue;
      }
      const bke::AttributeReader<float> masses = attributes.lookup_or_default<float>(
          sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);
      if (!masses) {
        continue;
      }
      threading::parallel_for(IndexRange(mesh.edges_num), 512, [&](const IndexRange range) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();
        for (const int edge_i : range) {
          const int2 edge = edges[edge_i];
          const int i0 = edge[0];
          const int i1 = edge[1];
          const float mass0 = masses.varray[i0];
          const float mass1 = masses.varray[i1];
          if (mass0 <= 0.0f || mass1 <= 0.0f) {
            continue;
          }
          solve_distance_constraint(geometry_i,
                                    geometry_i,
                                    i0,
                                    i1,
                                    positions[i0],
                                    positions[i1],
                                    1 / mass0,
                                    1 / mass1,
                                    compliance_term,
                                    rest_lengths.varray[edge_i],
                                    local_corrections);
        }
      });
    }
  }
};

class CurveConstraintSet : public ConstraintSet {
 protected:
  std::string self_path_;
  std::string filter_;
  std::string rest_length_attribute_;

 public:
  CurveConstraintSet(std::string self_path, std::string filter, std::string rest_length_attribute)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        rest_length_attribute_(std::move(rest_length_attribute))
  {
  }

  void ensure_rest_length(MutableSpan<SimGeometry> sim_geometries) const
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      Curves **curves_ptr = std::get_if<Curves *>(&sim_geometry.data);
      if (!curves_ptr) {
        continue;
      }
      Curves &curves_id = **curves_ptr;
      bke::CurvesGeometry &curves = curves_id.geometry.wrap();
      bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
      if (attributes.contains(rest_length_attribute_)) {
        continue;
      }
      bke::SpanAttributeWriter<float> rest_length_writer =
          attributes.lookup_or_add_for_write_only_span<float>(rest_length_attribute_,
                                                              bke::AttrDomain::Point);
      const Span<float3> positions = curves.positions();
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();
      const VArraySpan<bool> cyclic = curves.cyclic();
      threading::parallel_for(curves.curves_range(), 256, [&](IndexRange curves_range) {
        for (const int curve_i : curves_range) {
          const IndexRange points = points_by_curve[curve_i];
          if (points.size() < 2) {
            rest_length_writer.span.slice(points).fill(0.0f);
            continue;
          }
          for (const int point_i : points.drop_back(1)) {
            const int next_point_i = point_i + 1;
            const float3 &p0 = positions[point_i];
            const float3 &p1 = positions[next_point_i];
            const float length = math::distance(p0, p1);
            rest_length_writer.span[point_i] = length;
          }
          rest_length_writer.span[points.last()] = cyclic[curve_i] ?
                                                       math::distance(positions[points.last()],
                                                                      positions[0]) :
                                                       0.0f;
        }
      });
      rest_length_writer.finish();
    }
  }
};

class CurveLengthConstraintSet : public CurveConstraintSet {
 private:
  float compliance_;

 public:
  CurveLengthConstraintSet(std::string self_path,
                           std::string filter,
                           std::string rest_length_attribute,
                           const float compliance)
      : CurveConstraintSet(
            std::move(self_path), std::move(filter), std::move(rest_length_attribute)),
        compliance_(compliance)
  {
  }

  void ensure_init(const ConstraintContext & /*context*/,
                   MutableSpan<SimGeometry> sim_geometries) override
  {
    ensure_rest_length(sim_geometries);
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    float compliance_term = 0.0f;
    if (params.delta_time > 0.0f) {
      compliance_term = compliance_ / pow2f(params.delta_time);
    }
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      const Curves *const *curves_id = std::get_if<Curves *>(&sim_geometry.data);
      if (!curves_id) {
        continue;
      }
      const bke::CurvesGeometry &curves = (**curves_id).geometry.wrap();
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();
      const Span<float3> positions = curves.positions();
      const bke::AttributeAccessor attributes = curves.attributes();
      const bke::AttributeReader<float> rest_lengths = attributes.lookup<float>(
          rest_length_attribute_, bke::AttrDomain::Point);
      if (!rest_lengths) {
        continue;
      }
      const bke::AttributeReader<float> masses = attributes.lookup_or_default<float>(
          sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);
      if (!masses) {
        continue;
      }
      const VArray<bool> cyclic = curves.cyclic();
      threading::parallel_for(curves.curves_range(), 256, [&](IndexRange curves_range) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();
        for (const int curve_i : curves_range) {
          const IndexRange points = points_by_curve[curve_i];
          if (points.size() < 2) {
            continue;
          }
          for (const int point_i : points.drop_back(1)) {
            const int next_point_i = point_i + 1;
            const float mass0 = masses.varray[point_i];
            const float mass1 = masses.varray[next_point_i];
            if (mass0 <= 0.0f || mass1 <= 0.0f) {
              continue;
            }
            solve_distance_constraint(geometry_i,
                                      geometry_i,
                                      point_i,
                                      next_point_i,
                                      positions[point_i],
                                      positions[next_point_i],
                                      1 / mass0,
                                      1 / mass1,
                                      compliance_term,
                                      rest_lengths.varray[point_i],
                                      local_corrections);
          }
          if (cyclic[curve_i]) {
            const int first_point_i = points.first();
            const int last_point_i = points.last();
            const float mass0 = masses.varray[last_point_i];
            const float mass1 = masses.varray[first_point_i];
            if (mass0 <= 0.0f || mass1 <= 0.0f) {
              continue;
            }
            solve_distance_constraint(geometry_i,
                                      geometry_i,
                                      first_point_i,
                                      last_point_i,
                                      positions[last_point_i],
                                      positions[first_point_i],
                                      1 / mass0,
                                      1 / mass1,
                                      compliance_term,
                                      rest_lengths.varray[last_point_i],
                                      local_corrections);
          }
        }
      });
    }
  }
};

class CosseratRodConstraintSet : public CurveConstraintSet {
 protected:
 public:
  CosseratRodConstraintSet(std::string self_path,
                           std::string filter,
                           std::string rest_length_attribute)
      : CurveConstraintSet(
            std::move(self_path), std::move(filter), std::move(rest_length_attribute))
  {
  }

  void ensure_moment_of_inertia(MutableSpan<SimGeometry> sim_geometries) const
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      Curves **curves_ptr = std::get_if<Curves *>(&sim_geometry.data);
      if (!curves_ptr) {
        continue;
      }
      Curves &curves_id = **curves_ptr;
      bke::CurvesGeometry &curves = curves_id.geometry.wrap();
      bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
      BLI_assert(attributes.contains(rest_length_attribute_));
      if (!attributes.contains(sim_geometry.src.inertia_attribute)) {
        bke::SpanAttributeWriter<float3> inertia_writer =
            attributes.lookup_or_add_for_write_only_span<float3>(
                sim_geometry.src.inertia_attribute, bke::AttrDomain::Point);
        const VArraySpan<float> masses = *attributes.lookup_or_default<float>(
            sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);
        const VArraySpan<float> rest_distances = *attributes.lookup<float>(rest_length_attribute_,
                                                                           bke::AttrDomain::Point);
        const OffsetIndices points_by_curve = curves.points_by_curve();
        const VArraySpan<bool> cyclic = curves.cyclic();

        threading::parallel_for(curves.curves_range(), 512, [&](const IndexRange range) {
          for (const int curve_i : range) {
            const IndexRange points = points_by_curve[curve_i];
            if (points.size() < 2) {
              inertia_writer.span.slice(points).fill(float3(1.0f));
              continue;
            }

            auto inertia_from_points = [&](const int point0, const int point1) -> float3 {
              /* Thin rod model for moment-of-inertia.
               * All mass is located on the center line and evenly distributed,
               * leading to a moment of inertia of m*L^2/12 around X and Y.
               * Z component of inertia is negligible for thin rods. */
              const float mass0 = masses[point0];
              const float mass1 = masses[point1];
              const float rest_distance = rest_distances[point0];
              const float inertia = 0.08333f * (mass0 + mass1) * rest_distance * rest_distance;
              return float3(inertia, inertia, 0.0f);
            };

            for (const int point : points.drop_back(1)) {
              inertia_writer.span[point] = inertia_from_points(point, point + 1);
            }
            inertia_writer.span[points.last()] = cyclic[curve_i] ?
                                                     inertia_from_points(points.last(),
                                                                         points.first()) :
                                                     float3(0.0f);
          }
        });

        inertia_writer.finish();
      }
    }
  }
};

class RodLengthConstraintSet : public CosseratRodConstraintSet {
 private:
  float compliance_;

 public:
  RodLengthConstraintSet(std::string self_path,
                         std::string filter,
                         std::string rest_length_attribute,
                         const float compliance)
      : CosseratRodConstraintSet(
            std::move(self_path), std::move(filter), std::move(rest_length_attribute)),
        compliance_(compliance)
  {
  }

  void ensure_init(const ConstraintContext & /*context*/,
                   MutableSpan<SimGeometry> sim_geometries) override
  {
    ensure_rest_length(sim_geometries);
    ensure_moment_of_inertia(sim_geometries);
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    float compliance_term = 0.0f;
    if (params.delta_time > 0.0f) {
      compliance_term = compliance_ / pow2f(params.delta_time);
    }
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      const Curves *const *curves_id = std::get_if<Curves *>(&sim_geometry.data);
      if (!curves_id) {
        continue;
      }
      const bke::CurvesGeometry &curves = (**curves_id).geometry.wrap();
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();
      const bke::AttributeAccessor attributes = curves.attributes();

      const Span<float3> positions = curves.positions();
      const VArraySpan<math::Quaternion> rotations =
          *attributes.lookup_or_default<math::Quaternion>(sim_geometry.src.rotation_attribute,
                                                          bke::AttrDomain::Point,
                                                          math::Quaternion::identity());
      const VArraySpan<float> rest_lengths = *attributes.lookup_or_default<float>(
          rest_length_attribute_, bke::AttrDomain::Point, 1.0f);
      const VArraySpan<float> masses = *attributes.lookup_or_default<float>(
          sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);
      const VArraySpan<float3> inertias = *attributes.lookup_or_default<float3>(
          sim_geometry.src.inertia_attribute, bke::AttrDomain::Point, float3(1.0f));
      const VArray<bool> cyclic = curves.cyclic();

      auto solve_segment = [&](const int point_i, const int next_point_i) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();

        const float mass0 = masses[point_i];
        const float mass1 = masses[next_point_i];
        const float3 inertia = inertias[point_i];
        const float lumped_inertia = 0.5f * (inertia.x + inertia.y + inertia.z);
        /* Inverse mass as weight factors. */
        if (mass0 <= 0.0f || mass1 <= 0.0f || lumped_inertia <= 0.0f) {
          return;
        }
        const float rest_distance = rest_lengths[point_i];

        /* TODO carry over from previous iteration, use for warm-starting. */
        const float3 lambda_prev = float3(0.0f);

        solve_distance_rotation_constraint(geometry_i,
                                           geometry_i,
                                           geometry_i,
                                           point_i,
                                           next_point_i,
                                           point_i,
                                           positions[point_i],
                                           positions[next_point_i],
                                           rotations[point_i],
                                           1 / mass0,
                                           1 / mass1,
                                           1 / lumped_inertia,
                                           lambda_prev,
                                           compliance_term,
                                           rest_distance,
                                           local_corrections);
      };

      threading::parallel_for(curves.curves_range(), 256, [&](IndexRange curves_range) {
        for (const int curve_i : curves_range) {
          const IndexRange points = points_by_curve[curve_i];
          if (points.size() < 2) {
            continue;
          }
          for (const int point_i : points.drop_back(1)) {
            const int next_point_i = point_i + 1;
            solve_segment(point_i, next_point_i);
          }
          if (cyclic[curve_i]) {
            solve_segment(points.last(), points.first());
          }
        }
      });
    }
  }
};

class RodBendingConstraintSet : public CosseratRodConstraintSet {
 private:
  std::string rest_shape_attribute_;
  float compliance_;

 public:
  RodBendingConstraintSet(std::string self_path,
                          std::string filter,
                          std::string rest_length_attribute,
                          std::string rest_shape_attribute,
                          const float compliance)
      : CosseratRodConstraintSet(
            std::move(self_path), std::move(filter), std::move(rest_length_attribute)),
        rest_shape_attribute_(rest_shape_attribute),
        compliance_(compliance)
  {
  }

  /* Compute relative rotations from each segment to the next (aka. Darboux vectors). */
  void ensure_rest_shape(MutableSpan<SimGeometry> sim_geometries) const
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      Curves **curves_ptr = std::get_if<Curves *>(&sim_geometry.data);
      if (!curves_ptr) {
        continue;
      }
      Curves &curves_id = **curves_ptr;
      bke::CurvesGeometry &curves = curves_id.geometry.wrap();
      bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
      if (!attributes.contains(rest_shape_attribute_)) {
        bke::SpanAttributeWriter<math::Quaternion> rest_shape_writer =
            attributes.lookup_or_add_for_write_only_span<math::Quaternion>(rest_shape_attribute_,
                                                                           bke::AttrDomain::Point);
        const VArraySpan<math::Quaternion> rotations =
            *attributes.lookup_or_default<math::Quaternion>(sim_geometry.src.rotation_attribute,
                                                            bke::AttrDomain::Point,
                                                            math::Quaternion::identity());
        const OffsetIndices points_by_curve = curves.points_by_curve();
        const VArraySpan<bool> cyclic = curves.cyclic();

        threading::parallel_for(curves.curves_range(), 512, [&](const IndexRange range) {
          for (const int curve_i : range) {
            const IndexRange points = points_by_curve[curve_i];
            if (points.size() < 3) {
              rest_shape_writer.span.slice(points).fill(math::Quaternion::identity());
              continue;
            }

            auto rest_shape_from_points = [&](const int point0,
                                              const int point1) -> math::Quaternion {
              const math::Quaternion &rotation0 = rotations[point0];
              const math::Quaternion &rotation1 = rotations[point1];
              return math::invert_normalized(rotation0) * rotation1;
            };

            for (const int point : points.drop_back(1)) {
              rest_shape_writer.span[point] = rest_shape_from_points(point, point + 1);
            }
            rest_shape_writer.span[points.last()] = cyclic[curve_i] ?
                                                        rest_shape_from_points(points.last(),
                                                                               points.first()) :
                                                        math::Quaternion::identity();
          }
        });

        rest_shape_writer.finish();
      }
    }
  }

  void ensure_init(const ConstraintContext & /*context*/,
                   MutableSpan<SimGeometry> sim_geometries) override
  {
    ensure_moment_of_inertia(sim_geometries);
    ensure_rest_shape(sim_geometries);
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    float compliance_term = 0.0f;
    if (params.delta_time > 0.0f) {
      compliance_term = compliance_ / pow2f(params.delta_time);
    }
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      const Curves *const *curves_id = std::get_if<Curves *>(&sim_geometry.data);
      if (!curves_id) {
        continue;
      }
      const bke::CurvesGeometry &curves = (**curves_id).geometry.wrap();
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();
      const bke::AttributeAccessor attributes = curves.attributes();

      const VArraySpan<math::Quaternion> rotations =
          *attributes.lookup_or_default<math::Quaternion>(sim_geometry.src.rotation_attribute,
                                                          bke::AttrDomain::Point,
                                                          math::Quaternion::identity());
      const VArraySpan<math::Quaternion> rest_shapes =
          *attributes.lookup_or_default<math::Quaternion>(
              rest_shape_attribute_, bke::AttrDomain::Point, math::Quaternion::identity());
      const VArraySpan<float3> inertias = *attributes.lookup_or_default<float3>(
          sim_geometry.src.inertia_attribute, bke::AttrDomain::Point, float3(1.0f));
      const VArray<bool> cyclic = curves.cyclic();

      auto solve_segment = [&](const int point_i, const int next_point_i) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();

        const float3 inertia0 = inertias[point_i];
        const float3 inertia1 = inertias[next_point_i];
        const float lumped_inertia0 = 0.5f * (inertia0.x + inertia0.y + inertia0.z);
        const float lumped_inertia1 = 0.5f * (inertia1.x + inertia1.y + inertia1.z);
        /* Inverse inertia as weight factors. */
        if (lumped_inertia0 <= 0.0f || lumped_inertia1 <= 0.0f) {
          return;
        }
        const math::Quaternion &rest_shape = rest_shapes[point_i];

        /* TODO carry over from previous iteration, use for warm-starting. */
        const float4 lambda_prev = float4(0.0f);

        solve_bending_constraint(geometry_i,
                                 geometry_i,
                                 point_i,
                                 next_point_i,
                                 rotations[point_i],
                                 rotations[next_point_i],
                                 1 / lumped_inertia0,
                                 1 / lumped_inertia1,
                                 lambda_prev,
                                 compliance_term,
                                 rest_shape,
                                 local_corrections);
      };

      threading::parallel_for(curves.curves_range(), 256, [&](IndexRange curves_range) {
        for (const int curve_i : curves_range) {
          const IndexRange points = points_by_curve[curve_i];
          if (points.size() < 3) {
            continue;
          }
          for (const int point_i : points.drop_back(2)) {
            const int next_point_i = point_i + 1;
            solve_segment(point_i, next_point_i);
          }
          if (cyclic[curve_i]) {
            solve_segment(points.last() - 1, points.last());
            solve_segment(points.last(), points.first());
          }
        }
      });
    }
  }
};

class FixedPositionsConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  fn::Field<bool> selection_field_;
  fn::Field<float3> fixed_positions_field_;

 public:
  FixedPositionsConstraintSet(std::string self_path,
                              std::string filter,
                              fn::Field<bool> selection_field,
                              fn::Field<float3> fixed_positions_field)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        selection_field_(std::move(selection_field)),
        fixed_positions_field_(std::move(fixed_positions_field))
  {
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    this->foreach_fixed_position(
        params.sim_geometries,
        [&](const int geometry_i, const IndexMask &mask, const VArray<float3> &fixed_positions) {
          const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
          std::optional<bke::AttributeAccessor> attributes = sim_geometry.attributes();
          if (!attributes) {
            return;
          }
          const VArraySpan<float3> positions = *attributes->lookup<float3>("position");
          LocalConstraintCorrections &local_corrections = params.corrections.local();
          mask.foreach_index([&](const int i) {
            const float3 offset = fixed_positions[i] - positions[i];
            local_corrections.add_position_correction(geometry_i, i, offset);
          });
        });
  }

  void post_solve_apply(MutableSpan<SimGeometry> sim_geometries,
                        const PhysicsState * /*physics_state*/) override
  {
    this->foreach_fixed_position(
        sim_geometries,
        [&](const int geometry_i, const IndexMask &mask, const VArray<float3> &fixed_positions) {
          SimGeometry &sim_geometry = sim_geometries[geometry_i];
          std::optional<bke::MutableAttributeAccessor> attributes =
              sim_geometry.attributes_for_write();
          if (!attributes) {
            return;
          }
          bke::SpanAttributeWriter<float3> positions = attributes->lookup_for_write_span<float3>(
              "position");
          mask.foreach_index([&](const int i) { positions.span[i] = fixed_positions[i]; });
          positions.finish();
        });
  }

  void foreach_fixed_position(const Span<SimGeometry> sim_geometries,
                              FunctionRef<void(const int geometry_i,
                                               const IndexMask &mask,
                                               const VArray<float3> &fixed_positions)> fn) const
  {
    for (const int geometry_i : sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      std::optional<bke::GeometryFieldContext> field_context;
      const int points_num = sim_geometry.set_point_field_context(field_context);
      if (!field_context) {
        continue;
      }
      fn::FieldEvaluator field_evaluator{*field_context, points_num};
      field_evaluator.set_selection(selection_field_);
      field_evaluator.add(fixed_positions_field_);
      field_evaluator.evaluate();
      const IndexMask selection = field_evaluator.get_evaluated_selection_as_mask();
      if (selection.is_empty()) {
        continue;
      }
      const VArray<float3> fixed_positions = field_evaluator.get_evaluated<float3>(0);
      fn(geometry_i, selection, fixed_positions);
    }
  }
};

class FixedRotationsConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  fn::Field<bool> selection_field_;
  fn::Field<math::Quaternion> fixed_rotations_field_;
  float compliance_;

 public:
  FixedRotationsConstraintSet(std::string self_path,
                              std::string filter,
                              fn::Field<bool> selection_field,
                              fn::Field<math::Quaternion> fixed_rotations_field,
                              const float compliance)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        selection_field_(std::move(selection_field)),
        fixed_rotations_field_(std::move(fixed_rotations_field)),
        compliance_(compliance)
  {
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    float compliance_term = 0.0f;
    if (params.delta_time > 0.0f) {
      compliance_term = compliance_ / pow2f(params.delta_time);
    }
    this->foreach_fixed_rotation(
        params.sim_geometries,
        [&](const int geometry_i,
            const IndexMask &mask,
            const VArray<math::Quaternion> &fixed_rotations) {
          const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
          std::optional<bke::AttributeAccessor> attributes = sim_geometry.attributes();
          if (!attributes) {
            return;
          }
          const VArraySpan<math::Quaternion> rotations =
              *attributes->lookup_or_default<math::Quaternion>(sim_geometry.src.rotation_attribute,
                                                               bke::AttrDomain::Point,
                                                               math::Quaternion::identity());
          const VArraySpan<float3> inertias = *attributes->lookup_or_default<float3>(
              sim_geometry.src.inertia_attribute, bke::AttrDomain::Point, float3(1.0f));

          LocalConstraintCorrections &local_corrections = params.corrections.local();
          mask.foreach_index([&](const int point_i) {
            /* No relative offset, any additional rotation can be baked into the fixed rotation. */
            const math::Quaternion &rest_shape = math::Quaternion::identity();

            const float3 inertia1 = inertias[point_i];
            const float lumped_inertia1 = 0.5f * (inertia1.x + inertia1.y + inertia1.z);
            if (lumped_inertia1 <= 0.0f) {
              return;
            }

            /* TODO carry over from previous iteration, use for warm-starting. */
            const float4 lambda_prev = float4(0.0f);

            /* Use the generic bending constraint with 0/1 weight factors, so the 2nd rotation
             * receives the full correction while the 1st rotation remains fixed. */
            solve_bending_constraint(geometry_i,
                                     geometry_i,
                                     -1,
                                     point_i,
                                     fixed_rotations[point_i],
                                     rotations[point_i],
                                     0.0f,
                                     1 / lumped_inertia1,
                                     lambda_prev,
                                     compliance_term,
                                     rest_shape,
                                     local_corrections);
          });
        });
  }

  void foreach_fixed_rotation(
      const Span<SimGeometry> sim_geometries,
      FunctionRef<void(const int geometry_i,
                       const IndexMask &mask,
                       const VArray<math::Quaternion> &fixed_rotations)> fn) const
  {
    for (const int geometry_i : sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      std::optional<bke::GeometryFieldContext> field_context;
      const int points_num = sim_geometry.set_point_field_context(field_context);
      if (!field_context) {
        continue;
      }
      fn::FieldEvaluator field_evaluator{*field_context, points_num};
      field_evaluator.set_selection(selection_field_);
      field_evaluator.add(fixed_rotations_field_);
      field_evaluator.evaluate();
      const IndexMask selection = field_evaluator.get_evaluated_selection_as_mask();
      if (selection.is_empty()) {
        continue;
      }
      const VArray<math::Quaternion> fixed_rotations =
          field_evaluator.get_evaluated<math::Quaternion>(0);
      fn(geometry_i, selection, fixed_rotations);
    }
  }
};

class InfiniteCollisionPlaneConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  float3 position_;
  float3 normal_;

 public:
  InfiniteCollisionPlaneConstraintSet(std::string self_path,
                                      std::string filter,
                                      const float3 &position,
                                      const float3 &normal)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        position_(position),
        normal_(math::normalize(normal))
  {
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      std::optional<bke::AttributeAccessor> attributes = sim_geometry.attributes();
      if (!attributes) {
        continue;
      }
      const VArraySpan<float3> positions = *attributes->lookup<float3>("position");
      threading::parallel_for(positions.index_range(), 512, [&](const IndexRange range) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();
        for (const int i : range) {
          const float3 &position = positions[i];
          const float distance = math::dot(position - position_, normal_);
          if (distance >= 0.0f) {
            continue;
          }
          local_corrections.add_position_correction(geometry_i, i, normal_ * -distance);
        }
      });
    }
  }

  void post_solve_apply(MutableSpan<SimGeometry> sim_geometries,
                        const PhysicsState * /*physics_state*/) override
  {
    for (const int geometry_i : sim_geometries.index_range()) {
      SimGeometry &sim_geometry = sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      std::optional<bke::MutableAttributeAccessor> attributes =
          sim_geometry.attributes_for_write();
      if (!attributes) {
        continue;
      }
      bke::SpanAttributeWriter<float3> positions = attributes->lookup_for_write_span<float3>(
          "position");
      threading::parallel_for(positions.span.index_range(), 512, [&](const IndexRange range) {
        for (const int i : range) {
          float3 &position = positions.span[i];
          const float distance = math::dot(position - position_, normal_);
          if (distance >= 0.0f) {
            continue;
          }
          position -= normal_ * distance;
        }
      });
      positions.finish();
    }
  }
};

class GlobalVolumeConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  std::string rest_volume_name_;
  float overpressure_;

 public:
  GlobalVolumeConstraintSet(std::string self_path,
                            std::string filter,
                            std::string rest_volume_name,
                            const float overpressure = 1.0f)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        rest_volume_name_(std::move(rest_volume_name)),
        overpressure_(overpressure)
  {
  }

  void ensure_init(const ConstraintContext & /*context*/,
                   MutableSpan<SimGeometry> sim_geometries) override
  {
    for (SimGeometry &sim_geometry : sim_geometries) {
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      Mesh **mesh_ptr = std::get_if<Mesh *>(&sim_geometry.data);
      if (!mesh_ptr) {
        continue;
      }
      const Mesh &mesh = **mesh_ptr;
      if (sim_geometry.src.get_extra<float>(rest_volume_name_)) {
        continue;
      }
      const float volume = this->compute_volume(mesh);
      sim_geometry.src.set_extra<float>(rest_volume_name_, volume);
    }
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      const Mesh *const *mesh_ptr = std::get_if<Mesh *>(&sim_geometry.data);
      if (!mesh_ptr) {
        continue;
      }
      const Mesh &mesh = **mesh_ptr;
      if (mesh.verts_num == 0) {
        continue;
      }
      const std::optional<float> rest_volume = sim_geometry.src.get_extra<float>(
          rest_volume_name_);
      if (!rest_volume) {
        continue;
      }
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArray<float> masses = *attributes.lookup_or_default<float>(
          sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);

      const float current_volume = this->compute_volume(mesh);
      const float volume_diff = current_volume - overpressure_ * *rest_volume;

      Array<float3> gradients(mesh.verts_num, float3());
      const Span<int3> tris = mesh.corner_tris();
      const Span<float3> positions = mesh.vert_positions();
      const Span<int> corner_verts = mesh.corner_verts();
      threading::parallel_for(tris.index_range(), 512, [&](const IndexRange range) {
        for (const int tri_i : range) {
          const int3 &tri = tris[tri_i];
          const int v0 = corner_verts[tri[0]];
          const int v1 = corner_verts[tri[1]];
          const int v2 = corner_verts[tri[2]];
          const float3 &p0 = positions[v0];
          const float3 &p1 = positions[v1];
          const float3 &p2 = positions[v2];
          const float3 c_1_2 = math::cross(p1, p2);
          const float3 c_2_0 = math::cross(p2, p0);
          const float3 c_0_1 = math::cross(p0, p1);
          const float3 c = c_1_2 + c_2_0 + c_0_1;
          gradients[v0] += c;
          gradients[v1] += c;
          gradients[v2] += c;
        }
      });

      float lambda_divisor = 0.0f;
      for (const int i : IndexRange(mesh.verts_num)) {
        const float mass = masses[i];
        if (mass <= 0.0f) {
          continue;
        }
        lambda_divisor += math::length_squared(gradients[i]) / mass;
      }
      const float lambda = math::safe_divide(volume_diff, lambda_divisor);

      threading::parallel_for(IndexRange(mesh.verts_num), 512, [&](const IndexRange range) {
        LocalConstraintCorrections &local_corrections = params.corrections.local();
        for (const int i : range) {
          const float mass = masses[i];
          if (mass <= 0.0f) {
            continue;
          }
          const float3 offset = -lambda / mass * gradients[i];
          local_corrections.add_position_correction(geometry_i, i, offset);
        }
      });
    }
  }

  float compute_volume(const Mesh &mesh) const
  {
    const Span<int3> tris = mesh.corner_tris();
    const Span<int> corner_verts = mesh.corner_verts();
    const Span<float3> positions = mesh.vert_positions();

    const float volume = threading::parallel_deterministic_reduce<float>(
        tris.index_range(),
        512,
        0.0f,
        [&](const IndexRange range, float volume) {
          for (const int tri_i : range) {
            const int3 &tri = tris[tri_i];
            const int v0 = corner_verts[tri[0]];
            const int v1 = corner_verts[tri[1]];
            const int v2 = corner_verts[tri[2]];
            const float3 &p0 = positions[v0];
            const float3 &p1 = positions[v1];
            const float3 &p2 = positions[v2];
            volume += math::dot(math::cross(p0, p1), p2);
          }
          return volume;
        },
        [&](const float a, const float b) { return a + b; });
    return volume / 6.0f;
  }
};

class CollisionConstraintSet : public ConstraintSet {
 private:
  std::string self_path_;
  std::string filter_;
  fn::Field<bool> selection_field_;
  fn::Field<float> radius_field_;
  float speculative_contact_distance_;

 public:
  CollisionConstraintSet(std::string self_path,
                         std::string filter,
                         fn::Field<bool> selection_field,
                         fn::Field<float> radius_field,
                         const float speculative_contact_distance)
      : self_path_(std::move(self_path)),
        filter_(std::move(filter)),
        selection_field_(std::move(selection_field)),
        radius_field_(std::move(radius_field)),
        speculative_contact_distance_(speculative_contact_distance)
  {
  }

  void ensure_init(const ConstraintContext &context,
                   MutableSpan<SimGeometry> sim_geometries) override
  {
    if (!context.physics_state) {
      return;
    }

    const auto &jolt_state = *static_cast<jolt_physics::JoltState *>(context.physics_state);
    const JPH::SphereShapeSettings sphere_shape_settings{1.0f};
    const JPH::Shape *sphere_shape = sphere_shape_settings.Create().Get();
    BLI_assert(sphere_shape != nullptr);

    for (SimGeometry &sim_geometry : sim_geometries) {
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      std::optional<bke::AttributeAccessor> attributes = sim_geometry.attributes();
      if (!attributes) {
        continue;
      }

      std::optional<bke::GeometryFieldContext> field_context;
      const int points_num = sim_geometry.set_point_field_context(field_context);
      if (!field_context) {
        continue;
      }
      fn::FieldEvaluator field_evaluator{*field_context, points_num};
      field_evaluator.set_selection(selection_field_);
      field_evaluator.add(radius_field_);
      field_evaluator.evaluate();
      const IndexMask selection = field_evaluator.get_evaluated_selection_as_mask();
      if (selection.is_empty()) {
        continue;
      }
      const VArraySpan<float> radii = field_evaluator.get_evaluated<float>(0);
      const VArraySpan<float3> positions = *attributes->lookup<float3>("position");

      /* TODO doing so many individual shape collisions is inefficient due to repeated broad phase
       * tree traversal. Replace this a compound shape so a single collision query can be used for
       * the entire hair system. */
      /* NOTE: isolated single threaded execution to make it easier to safely append contacts to
       * the combined set. */
      /* NOTE2: We set the sub_shape_id1 to identify the point doing the collision query. With a
       * compound shape, where each sub shape represents a hair point or segment, the sub-shape ID
       * would be just that for primitive shapes. Here's an explanation of the meaning of
       * sub-shape IDs in contacts by jrouwe:
       * https://github.com/jrouwe/JoltPhysics/discussions/473#discussioncomment-5314813*/
      jolt_physics::ContactPoints contacts;
      threading::isolate_task([&]() {
        selection.foreach_index([&](const int index) {
          const float safe_radius = std::max(radii[index], 0.001f);
          const float4x4 transform = math::from_loc_scale<float4x4>(positions[index],
                                                                    float3(safe_radius));
          const int prev_contacts_num = contacts.body_id2.size();
          jolt_physics::collide_shape(
              jolt_state, *sphere_shape, transform, speculative_contact_distance_, contacts);
          /* Initialize sub-shape ID to identify the colliding point. */
          const IndexRange new_contacts = contacts.body_id2.index_range().drop_front(
              prev_contacts_num);
          contacts.subshape_id1.as_mutable_span().slice(new_contacts).fill(index);
        });
      });

      bke::GeometrySet prev_contact_points = sim_geometry.src
                                                 .get_extra<bke::GeometrySet>("Contact Points")
                                                 .value_or(bke::GeometrySet{});
      bke::GeometrySet contact_points = pointcloud_from_contacts(std::move(contacts));
      sim_geometry.src.set_extra(
          "Contact Points",
          join_geometries({std::move(prev_contact_points), std::move(contact_points)}, {}));
    }
  }

  /**
   * Positional contact constraint based on
   * "Detailed Rigid Body Simulation with Extended Position Based Dynamics", Mueller et al., 2020
   */
  static float3 collision_response(const float3 &axis, const float depth)
  {
    return math::safe_divide(-axis * depth, math::length_squared(axis));
  }

  /**
   * Handle each contact point in a callback.
   * \param fn Callback function handling contacts
   *   fn(int point_index, bool active, const float3 &offset)
   */
  template<typename Fn>
  void foreach_contact(const SimGeometry &sim_geometry,
                       const JPH::BodyInterface &body_interface,
                       Fn fn)
  {
    const std::optional<bke::AttributeAccessor> attributes = sim_geometry.attributes();
    if (!attributes) {
      return;
    }
    const VArraySpan<float3> positions = *attributes->lookup<float3>("position",
                                                                     bke::AttrDomain::Point);
    const IndexRange points = positions.index_range();

    const bke::GeometrySet contacts = sim_geometry.src
                                          .get_extra<bke::GeometrySet>("Contact Points")
                                          .value_or(bke::GeometrySet{});
    const PointCloud *contact_pointcloud = contacts.get_pointcloud();
    if (!contact_pointcloud || contact_pointcloud->totpoint == 0) {
      return;
    }
    const bke::AttributeAccessor contact_attributes = contact_pointcloud->attributes();
    const VArraySpan<int> subshape_ids1 = *contact_attributes.lookup<int>(
        ContactPointAttributeNames::subshape_id1, bke::AttrDomain::Point);
    const VArraySpan<int> body_ids2 = *contact_attributes.lookup<int>(
        ContactPointAttributeNames::body_id2, bke::AttrDomain::Point);
    const VArraySpan<float3> contact_points1 = *contact_attributes.lookup<float3>(
        ContactPointAttributeNames::contact_point1, bke::AttrDomain::Point);
    const VArraySpan<float3> contact_points2 = *contact_attributes.lookup<float3>(
        ContactPointAttributeNames::contact_point2, bke::AttrDomain::Point);
    const VArraySpan<float3> penetration_axes = *contact_attributes.lookup<float3>(
        ContactPointAttributeNames::penetration_axis, bke::AttrDomain::Point);

    threading::parallel_for(contact_points1.index_range(), 512, [&](const IndexRange range) {
      for (const int contact_i : range) {
        const int point_index = subshape_ids1[contact_i];
        if (!points.contains(point_index)) {
          continue;
        }
        const JPH::BodyID body_id2 = JPH::BodyID(uint32_t(body_ids2[contact_i]));

        const float3 &body1_position = positions[point_index];
        JPH::RVec3 jolt_body2_position;
        JPH::Quat jolt_body2_rotation;
        body_interface.GetPositionAndRotation(body_id2, jolt_body2_position, jolt_body2_rotation);
        const float3 body2_position = jolt_physics::convert_vec3(jolt_body2_position);
        const math::Quaternion body2_rotation = jolt_physics::convert_quat(jolt_body2_rotation);

        /* World space contact points are computed by applying the collider transforms to local
         * contact positions. */
        const float3 contact_point1 = contact_points1[contact_i] + body1_position;
        const float3 contact_point2 = math::transform_point(body2_rotation,
                                                            contact_points2[contact_i]) +
                                      body2_position;
        /* Note: Axis is not normalized, depth is relative to length of this vector. */
        const float3 penetration_axis = penetration_axes[contact_i];

        /* Positional constraint for depth along the penetration axis. */
        const float residual_depth = math::dot(contact_point1 - contact_point2, penetration_axis);
        /* Only act on contact. */
        const bool active = residual_depth > 0.0f;
        fn(point_index, active, penetration_axis, residual_depth);

        /* TODO previous implementation including rotation correction. */
        // // /* Effective mass correction to account for the effect of rotation on position
        // //  * displacement. See section 3.3.1 "Positional Constraints" of the paper. We use a
        // //  * simplified rotational weight factor instead of full inverse moment of inertia.
        // */
        // // const float rot_factor1 = math::length_squared(local_position1) -
        // //                           math::square(math::dot(local_position1, normal));
        // // const float rot_factor2 = math::length_squared(local_position2) -
        // //                           math::square(math::dot(local_position2, normal));
        // // const float weight_norm = math::safe_rcp(weight_pos1 + weight_rot1 * rot_factor1 +
        // //                                          weight_pos2 + weight_rot2 * rot_factor2 +
        // //                                          alpha);
        // /* Gradient is normal, length is 1, no need to compute gradient norm. */
        // r_delta_lambda = weight_norm * (-r_residual_depth - alpha * lambda);
        // const float3 impulse1 = r_delta_lambda * normal;
        // const float3 impulse2 = -impulse1;
        // r_delta_position1 = impulse1 * weight_pos1;
        // r_delta_position2 = impulse2 * weight_pos2;
        // r_delta_rotation1 = float4(0.0f, math::cross(local_position1, impulse1)) *
        // weight_rot1; r_delta_rotation2 = float4(0.0f, math::cross(local_position2, impulse2))
        // * weight_rot2;
      }
    });
  }

  void solve(ConstraintSetSolveParams &params) override
  {
    if (!params.physics_state) {
      return;
    }
    const auto &jolt_state = static_cast<const jolt_physics::JoltState &>(*params.physics_state);
    const JPH::BodyInterface &body_interface = jolt_state.system.GetBodyInterfaceNoLock();

    LocalConstraintCorrections &local_corrections = params.corrections.local();
    for (const int geometry_i : params.sim_geometries.index_range()) {
      const SimGeometry &sim_geometry = params.sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }

      foreach_contact(
          sim_geometry,
          body_interface,
          [&](const int point_index, const bool active, const float3 &axis, const float depth) {
            if (active) {
              local_corrections.add_position_correction(
                  geometry_i, point_index, collision_response(axis, depth));
            }
          });
    }
  }

  void post_solve_apply(MutableSpan<SimGeometry> sim_geometries,
                        const PhysicsState *physics_state) override
  {
    if (!physics_state) {
      return;
    }
    const auto &jolt_state = *static_cast<const jolt_physics::JoltState *>(physics_state);
    const JPH::BodyInterface &body_interface = jolt_state.system.GetBodyInterfaceNoLock();

    for (const int geometry_i : sim_geometries.index_range()) {
      SimGeometry &sim_geometry = sim_geometries[geometry_i];
      if (!nested_bundle_path_is_selected(self_path_, filter_, sim_geometry.src.path)) {
        continue;
      }
      std::optional<bke::MutableAttributeAccessor> attributes =
          sim_geometry.attributes_for_write();
      if (!attributes) {
        continue;
      }

      bke::SpanAttributeWriter<float3> positions = attributes->lookup_for_write_span<float3>(
          "position");
      foreach_contact(
          sim_geometry,
          body_interface,
          [&](const int point_index, const bool active, const float3 &axis, const float depth) {
            if (active) {
              positions.span[point_index] += collision_response(axis, depth);
            }
          });
      positions.finish();
    }
  }
};

ConstraintSet &create_constraint__edge_lengths(ResourceScope &scope,
                                               std::string self_path,
                                               std::string filter,
                                               std::string rest_length_attribute,
                                               const float compliance)
{
  return scope.construct<EdgeLengthConstraintSet>(
      std::move(self_path), std::move(filter), std::move(rest_length_attribute), compliance);
}

ConstraintSet &create_constraint__curve_lengths(ResourceScope &scope,
                                                std::string self_path,
                                                std::string filter,
                                                std::string rest_length_attribute,
                                                float compliance)
{
  return scope.construct<CurveLengthConstraintSet>(
      self_path, filter, std::move(rest_length_attribute), compliance);
}

ConstraintSet &create_constraint__cosserat_rod_lengths(ResourceScope &scope,
                                                       std::string self_path,
                                                       std::string filter,
                                                       std::string rest_length_attribute,
                                                       float compliance)
{
  return scope.construct<RodLengthConstraintSet>(
      self_path, filter, std::move(rest_length_attribute), compliance);
}

ConstraintSet &create_constraint__cosserat_rod_bending(ResourceScope &scope,
                                                       std::string self_path,
                                                       std::string filter,
                                                       std::string rest_length_attribute,
                                                       std::string rest_shape_attribute,
                                                       float compliance)
{
  return scope.construct<RodBendingConstraintSet>(self_path,
                                                  filter,
                                                  std::move(rest_length_attribute),
                                                  std::move(rest_shape_attribute),
                                                  compliance);
}

ConstraintSet &create_constraint__fixed_positions(ResourceScope &scope,
                                                  std::string self_path,
                                                  std::string filter,
                                                  fn::Field<bool> selection_field,
                                                  fn::Field<float3> fixed_positions_field)
{
  return scope.construct<FixedPositionsConstraintSet>(
      self_path, filter, std::move(selection_field), std::move(fixed_positions_field));
}

ConstraintSet &create_constraint__fixed_rotations(
    ResourceScope &scope,
    std::string self_path,
    std::string filter,
    fn::Field<bool> selection_field,
    fn::Field<math::Quaternion> fixed_rotations_field,
    const float compliance)
{
  return scope.construct<FixedRotationsConstraintSet>(
      self_path, filter, std::move(selection_field), std::move(fixed_rotations_field), compliance);
}

ConstraintSet &create_constraint__infinite_collision_plane(ResourceScope &scope,
                                                           std::string self_path,
                                                           std::string filter,
                                                           const float3 &position,
                                                           const float3 &normal)
{
  return scope.construct<InfiniteCollisionPlaneConstraintSet>(self_path, filter, position, normal);
}

ConstraintSet &create_constraint__global_volume(ResourceScope &scope,
                                                std::string self_path,
                                                std::string filter,
                                                std::string rest_volume_name,
                                                const float overpressure)
{
  return scope.construct<GlobalVolumeConstraintSet>(
      self_path, filter, std::move(rest_volume_name), overpressure);
}

ConstraintSet &create_constraint__collision(ResourceScope &scope,
                                            std::string self_path,
                                            std::string filter,
                                            fn::Field<bool> selection_field,
                                            fn::Field<float> radius_field,
                                            const float speculative_contact_distance)
{
  return scope.construct<CollisionConstraintSet>(self_path,
                                                 filter,
                                                 std::move(selection_field),
                                                 std::move(radius_field),
                                                 speculative_contact_distance);
}

static void prepare_sim_geometry(SimGeometry &sim_geometry)
{
  std::optional<bke::MutableAttributeAccessor> attributes = sim_geometry.attributes_for_write();
  /* Remember previous positions. */
  if (attributes) {
    const bke::AttributeReader<float3> positions = attributes->lookup<float3>("position");
    attributes->remove(prev_position_name);
    attributes->add<float3>(prev_position_name,
                            bke::AttrDomain::Point,
                            bke::AttributeInitShared{positions.varray.get_internal_span().data(),
                                                     *positions.sharing_info});

    if (const bke::AttributeReader<math::Quaternion> rotations =
            attributes->lookup<math::Quaternion>(sim_geometry.src.rotation_attribute))
    {

      attributes->remove(prev_rotation_name);
      attributes->add<math::Quaternion>(
          prev_rotation_name,
          bke::AttrDomain::Point,
          bke::AttributeInitShared{rotations.varray.get_internal_span().data(),
                                   *rotations.sharing_info});
    }
  }

  /* Clear contact points. */
  sim_geometry.src.remove_extra("Contact Points");
}

static void cleanup_sim_geometry(SimGeometry &sim_geometry)
{
  std::optional<bke::MutableAttributeAccessor> attributes = sim_geometry.attributes_for_write();
  if (!attributes) {
    return;
  }
  attributes->remove(prev_position_name);
  attributes->remove(prev_rotation_name);
}

static void dynamics_time_step(const Behaviors &behaviors,
                               SimGeometry &sim_geometry,
                               const float delta_time)
{
  std::optional<bke::MutableAttributeAccessor> attributes = sim_geometry.attributes_for_write();
  if (!attributes) {
    return;
  }
  std::optional<bke::GeometryFieldContext> field_context;
  sim_geometry.set_point_field_context(field_context);
  if (!field_context) {
    return;
  }
  const int positions_num = attributes->domain_size(bke::AttrDomain::Point);

  Vector<const ForceField *> filtered_force_fields;
  for (const ForceField &sim_force : behaviors.force_fields) {
    if (nested_bundle_path_is_selected(
            sim_force.self_path, sim_force.filter, sim_geometry.src.path))
    {
      filtered_force_fields.append(&sim_force);
    }
  }
  Vector<const AccelerationField *> filtered_acceleration_fields;
  for (const AccelerationField &sim_acceleration : behaviors.acceleration_fields) {
    if (nested_bundle_path_is_selected(
            sim_acceleration.self_path, sim_acceleration.filter, sim_geometry.src.path))
    {
      filtered_acceleration_fields.append(&sim_acceleration);
    }
  }

  Array<float3> force(positions_num, float3());
  Array<float3> acceleration(positions_num, float3());
  fn::FieldEvaluator field_evaluator{*field_context, positions_num};
  for (const ForceField *sim_force : filtered_force_fields) {
    field_evaluator.add(sim_force->force_field);
  }
  for (const AccelerationField *sim_acceleration : filtered_acceleration_fields) {
    field_evaluator.add(sim_acceleration->acceleration_field);
  }
  field_evaluator.evaluate();
  for (const int force_i : filtered_force_fields.index_range()) {
    VArraySpan<float3> force_varray = field_evaluator.get_evaluated<float3>(force_i);
    for (const int i : force_varray.index_range()) {
      force[i] += force_varray[i];
    }
  }
  for (const int acceleration_i : filtered_acceleration_fields.index_range()) {
    VArraySpan<float3> acceleration_varray = field_evaluator.get_evaluated<float3>(
        acceleration_i + filtered_force_fields.size());
    for (const int i : acceleration_varray.index_range()) {
      acceleration[i] += acceleration_varray[i];
    }
  }
  const VArray<float> masses = *attributes->lookup_or_default<float>(
      sim_geometry.src.mass_attribute, bke::AttrDomain::Point, 1.0f);
  bke::SpanAttributeWriter<float3> velocities = attributes->lookup_or_add_for_write_span<float3>(
      sim_geometry.src.velocity_attribute, bke::AttrDomain::Point);
  bke::SpanAttributeWriter<float3> positions = attributes->lookup_or_add_for_write_span<float3>(
      "position", bke::AttrDomain::Point);

  threading::parallel_for(velocities.span.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      acceleration[i] += force[i] / masses[i];
      velocities.span[i] += acceleration[i] * delta_time;
    }
  });
  /* Apply velocity damping before position integration. */
  for (const Damping &damping : behaviors.dampings) {
    if (nested_bundle_path_is_selected(damping.self_path, damping.filter, sim_geometry.src.path)) {
      const float damping_factor = 1.0f - damping.linear_damping * delta_time;
      if (damping_factor <= 0.0f) {
        continue;
      }
      threading::parallel_for(velocities.span.index_range(), 1024, [&](const IndexRange range) {
        for (const int i : range) {
          velocities.span[i] *= damping_factor;
        }
      });
    }
  }
  threading::parallel_for(positions.span.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      positions.span[i] += velocities.span[i] * delta_time;
    }
  });

  velocities.finish();
  positions.finish();

  /* Rotational dynamics, if a valid rotation attribute exists. */
  const auto rotation_meta_data = attributes->lookup_meta_data(
      sim_geometry.src.rotation_attribute);
  if (rotation_meta_data && rotation_meta_data->data_type == bke::AttrType::Quaternion &&
      rotation_meta_data->domain == bke::AttrDomain::Point)
  {
    const VArray<float3> inertias = *attributes->lookup_or_default<float3>(
        sim_geometry.src.inertia_attribute, bke::AttrDomain::Point, float3(1.0f));
    bke::SpanAttributeWriter<math::Quaternion> rotations =
        attributes->lookup_or_add_for_write_span<math::Quaternion>(
            sim_geometry.src.rotation_attribute, bke::AttrDomain::Point);
    bke::SpanAttributeWriter<float3> angular_velocities =
        attributes->lookup_or_add_for_write_span<float3>(
            sim_geometry.src.angular_velocity_attribute, bke::AttrDomain::Point);

    threading::parallel_for(
        angular_velocities.span.index_range(), 1024, [&](const IndexRange range) {
          for (const int i : range) {
            const float3 &inertia = inertias[i];
            float3 &angular_velocity = angular_velocities.span[i];
            /* TODO eventually may have external "torque fields", ignore for now. */
            const float3 external_torque = float3(0.0f);
            const float3 precession = math::cross(angular_velocity, angular_velocity * inertia);

            angular_velocity += delta_time *
                                (math::safe_divide(external_torque - precession, inertia));
          }
        });
    /* Apply angular velocity damping before rotation integration. */
    for (const Damping &damping : behaviors.dampings) {
      if (nested_bundle_path_is_selected(damping.self_path, damping.filter, sim_geometry.src.path))
      {
        const float damping_factor = 1.0f - damping.angular_damping * delta_time;
        if (damping_factor <= 0.0f) {
          continue;
        }
        threading::parallel_for(
            angular_velocities.span.index_range(), 1024, [&](const IndexRange range) {
              for (const int i : range) {
                angular_velocities.span[i] *= damping_factor;
              }
            });
      }
    }
    threading::parallel_for(rotations.span.index_range(), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        const float3 &angular_velocity = angular_velocities.span[i];
        math::Quaternion &rotation = rotations.span[i];
        const math::Quaternion direction = rotation * math::Quaternion(0, angular_velocity);
        rotation = math::normalize(
            math::Quaternion(float4(rotation) + delta_time * 0.5f * float4(direction)));
      }
    });

    rotations.finish();
    angular_velocities.finish();
  }
}

static void estimate_velocities(SimGeometry &sim_geometry, const float delta_time)
{
  std::optional<bke::MutableAttributeAccessor> attributes = sim_geometry.attributes_for_write();
  if (!attributes) {
    return;
  }
  const VArraySpan<float3> prev_positions = *attributes->lookup<float3>(prev_position_name);
  const VArraySpan<float3> positions = *attributes->lookup<float3>("position");
  bke::SpanAttributeWriter<float3> velocities = attributes->lookup_for_write_span<float3>(
      sim_geometry.src.velocity_attribute);
  threading::parallel_for(positions.index_range(), 512, [&](const IndexRange range) {
    for (const int i : range) {
      velocities.span[i] = (positions[i] - prev_positions[i]) / delta_time;
    }
  });
  velocities.finish();

  /* Rotational dynamics, if a valid rotation attribute exists. */
  const auto rotation_meta_data = attributes->lookup_meta_data(
      sim_geometry.src.rotation_attribute);
  if (rotation_meta_data && rotation_meta_data->data_type == bke::AttrType::Quaternion &&
      rotation_meta_data->domain == bke::AttrDomain::Point)
  {
    const VArraySpan<math::Quaternion> prev_rotations = *attributes->lookup<math::Quaternion>(
        prev_rotation_name);
    const VArraySpan<math::Quaternion> rotations = *attributes->lookup<math::Quaternion>(
        sim_geometry.src.rotation_attribute);
    bke::SpanAttributeWriter<float3> angular_velocities =
        attributes->lookup_or_add_for_write_span<float3>(
            sim_geometry.src.angular_velocity_attribute, bke::AttrDomain::Point);
    threading::parallel_for(rotations.index_range(), 512, [&](const IndexRange range) {
      for (const int i : range) {
        angular_velocities.span[i] =
            2.0f * (math::invert_normalized(prev_rotations[i]) * rotations[i]).imaginary_part() /
            delta_time;
      }
    });
    angular_velocities.finish();
  }
}

static void solve_substep(Behaviors &behaviors,
                          MutableSpan<SimGeometry> sim_geometries,
                          const float sub_delta_time)
{
  jolt_physics::JoltState *jolt_state = static_cast<jolt_physics::JoltState *>(
      behaviors.physics_state);

  for (SimGeometry &sim_geometry : sim_geometries) {
    prepare_sim_geometry(sim_geometry);
  }

  /* Init constraints. */
  const ConstraintContext constraint_contact{jolt_state};
  for (ConstraintSet *constraint : behaviors.constraint_sets) {
    constraint->ensure_init(constraint_contact, sim_geometries);
  }

  /* Handle forces and accelerations. If the time step is zero, these can't have any effect. */
  if (sub_delta_time > 0) {
    for (SimGeometry &sim_geometry : sim_geometries) {
      dynamics_time_step(behaviors, sim_geometry, sub_delta_time);
    }
  }

  /* Constraint solve step. */
  ConstraintCorrections corrections(sim_geometries);
  ConstraintSetSolveParams params{sub_delta_time, sim_geometries, jolt_state, corrections};
  threading::parallel_for(behaviors.constraint_sets.index_range(), 1, [&](const IndexRange range) {
    for (const int constraint_i : range) {
      ConstraintSet *constraints = behaviors.constraint_sets[constraint_i];
      constraints->solve(params);
    }
  });

  corrections.apply();

  /* Apply hard constraints. */
  for (ConstraintSet *constraint : behaviors.constraint_sets) {
    constraint->post_solve_apply(sim_geometries, jolt_state);
  }

  /* Write back velocities. Velocities can't be computed if the time step is zero. */
  if (sub_delta_time > 0) {
    for (SimGeometry &sim_geometry : sim_geometries) {
      estimate_velocities(sim_geometry, sub_delta_time);
    }
  }

  /* Remove temporary attributes.*/
  for (SimGeometry &sim_geometry : sim_geometries) {
    cleanup_sim_geometry(sim_geometry);
  }
}

/**
 * Jolt step listener that runs a XPBD solver substep in sync with the Jolt update.
 */
class XPBDJoltStepListener : public JPH::PhysicsStepListener {
  Behaviors &behaviors_;
  MutableSpan<SimGeometry> sim_geometries_;

 public:
  XPBDJoltStepListener(Behaviors &behaviors, MutableSpan<SimGeometry> sim_geometries)
      : behaviors_(behaviors), sim_geometries_(sim_geometries)
  {
  }
  ~XPBDJoltStepListener() {}

  /* The OnStep callback is called at the beginning of a time step. */
  void OnStep(const JPH::PhysicsStepListenerContext &context) override
  {
    /* Nothing to do before the first step. */
    if (context.mIsFirstStep) {
      return;
    }

    solve_substep(behaviors_, sim_geometries_, context.mDeltaTime);
  }
};

void solve(Behaviors &behaviors, const float total_delta_time, const int substeps)
{
  BLI_assert(total_delta_time >= 0.0f);
  /* Limit substeps to avoid crashing in extreme cases. */
  const int sim_steps = std::clamp(1 + substeps, 1, 240);

  jolt_physics::JoltState *jolt_state = static_cast<jolt_physics::JoltState *>(
      behaviors.physics_state);

  Vector<SimGeometry> sim_geometries;
  for (SimGeometrySet *sim_geometry_set : behaviors.sim_geometry_sets) {
    if (Mesh *mesh = sim_geometry_set->geometry.get_mesh_for_write()) {
      sim_geometries.append(SimGeometry{*sim_geometry_set, mesh});
    }
    if (PointCloud *pointcloud = sim_geometry_set->geometry.get_pointcloud_for_write()) {
      sim_geometries.append(SimGeometry{*sim_geometry_set, pointcloud});
    }
    if (Curves *curves = sim_geometry_set->geometry.get_curves_for_write()) {
      sim_geometries.append(SimGeometry{*sim_geometry_set, curves});
    }
  }

  if (jolt_state) {
    /* The Jolt state can't easily be reset to an older state. So better just don't do simulation
     * in this case. */
    const bool is_resimulating = (behaviors.update_counter < jolt_state->update_counter);
    behaviors.update_counter++;
    if (!is_resimulating) {
      update_jolt_state_from_behaviors(*jolt_state, behaviors);

      /* Use a JPH::PhysicsStepListener to synchronize our XPBD softbody solver substeps with
       * the Jolt system substeps (collision steps). That way we get accurate positions of moving
       * rigid bodies (dynamic or animated) for collision detection instead of quasi-static
       * collision shapes that only move on every full frame step. */
      XPBDJoltStepListener jolt_step_listener(behaviors, sim_geometries);
      jolt_state->system.AddStepListener(&jolt_step_listener);
      {
        JPH::TempAllocatorImpl temp_allocator(10 * 1024 * 1024);
        const int collision_steps = sim_steps;
        jolt_state->system.Update(
            total_delta_time, collision_steps, &temp_allocator, &(*jolt_state->job_system));
      }
      jolt_state->system.RemoveStepListener(&jolt_step_listener);
      /* Last substep at the end of the Jolt collision steps. This is not covered by the step
       * listener because it gets called at the beginning instead of the end of each substep. */
      solve_substep(behaviors, sim_geometries, total_delta_time / sim_steps);

      jolt_state->update_counter = behaviors.update_counter;
    }
  }
  else {
    /* Own substep loop if Jolt isn't used. */
    const float sub_delta_time = total_delta_time / sim_steps;
    for ([[maybe_unused]] int substep : IndexRange(sim_steps)) {
      solve_substep(behaviors, sim_geometries, sub_delta_time);
    }
  }

  /* Update final instance transforms and softbody mesh shapes. */
  if (jolt_state) {
    for (RigidBodyInstances &rigid_body_instances : behaviors.rigid_body_instances) {
      rigid_body_instances.instances_geometry = apply_rigid_body_simulation(rigid_body_instances,
                                                                            *jolt_state);
    }
    for (SoftBodyMesh &soft_body_mesh : behaviors.soft_body_meshes) {
      soft_body_mesh.mesh_geometry = apply_soft_body_simulation(soft_body_mesh, *jolt_state);
    }
  }
}

}  // namespace blender::geometry::xpbd_old
