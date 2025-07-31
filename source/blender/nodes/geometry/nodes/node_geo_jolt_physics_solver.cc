/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances.hh"
#include "BLI_generic_key.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"
#include "BLI_task.h"
#include "BLI_threads.h"
#include "DNA_mesh_types.h"
#include "GEO_shape_hash.hh"
#include "NOD_geometry_nodes_behaviors.hh"
#include "NOD_geometry_nodes_behaviors_common.hh"
#include "NOD_geometry_nodes_bundle.hh"

#include "node_geometry_util.hh"

#include "Jolt/Jolt.h"
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/RegisterTypes.h>

namespace blender::nodes::node_geo_jolt_physics_solver_cc {

static std::shared_ptr<BehaviorListDef> make_behavior_list_def()
{
  auto list = std::make_shared<BehaviorListDef>();
  list->behaviors.add(GravityBehavior::def());
  list->behaviors.add(ForceBehavior::def());
  list->behaviors.add(RigidBodyInstancesBehavior::def());
  list->behaviors.add(SoftBodyMeshBehavior::def());
  return list;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  static std::shared_ptr<const BehaviorListDef> behaviors = make_behavior_list_def();

  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bundle>("Data");
  b.add_output<decl::Bundle>("Data").align_with_previous();
  b.add_input<decl::Bundle>("Behavior").behaviors(behaviors);
  b.add_input<decl::Float>("Delta Time").min(0).hide_value();
  b.add_input<decl::Int>("Substeps").default_value(1).min(1);
}

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
  const GeometrySet &geometry;
  const float3 &scale;
  const float density;
};

struct CollisionShapeCache {
  struct CachedShape {
    JPH::ShapeSettings::ShapeResult shape;
    bool still_used = true;

    CachedShape(JPH::ShapeSettings::ShapeResult shape = {}) : shape(std::move(shape)) {}
  };

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

  Map<BoxID, CachedShape> boxes;
  Map<SphereID, CachedShape> spheres;
  Map<ConvexHullID, CachedShape> convex_hulls;

  void reset_used()
  {
    for (CachedShape &cached_shape : this->boxes.values()) {
      cached_shape.still_used = false;
    }
    for (CachedShape &cached_shape : this->spheres.values()) {
      cached_shape.still_used = false;
    }
    for (CachedShape &cached_shape : this->convex_hulls.values()) {
      cached_shape.still_used = false;
    }
  }

  void remove_unused()
  {
    this->boxes.remove_if([](const auto &item) { return !item.value.still_used; });
    this->spheres.remove_if([](const auto &item) { return !item.value.still_used; });
    this->convex_hulls.remove_if([](const auto &item) { return !item.value.still_used; });
  }

  JPH::ShapeSettings::ShapeResult get_or_create_box(const CollisionShapeParams &params)
  {
    const std::optional<Bounds<float3>> bounds =
        params.geometry.compute_boundbox_without_instances(true);
    if (!bounds) {
      return {};
    }
    const float3 half_extent = math::max(math::abs(bounds->min), math::abs(bounds->max)) *
                               params.scale;
    return this->get_or_create_box(half_extent, params.density);
  }

  JPH::ShapeSettings::ShapeResult get_or_create_box(const float3 &half_extent, const float density)
  {
    const BoxID box_id{half_extent, density};
    CachedShape &cached_shape = this->boxes.lookup_or_add_cb(box_id, [&]() {
      JPH::BoxShapeSettings box_shape_settings{convert_vec3(half_extent)};
      box_shape_settings.mDensity = density;
      const float min_axis = std::min({half_extent.x, half_extent.y, half_extent.z});
      box_shape_settings.mConvexRadius = 0.5 * std::min(min_axis, 0.1f);
      return box_shape_settings.Create();
    });
    if (cached_shape.shape.IsValid()) {
      cached_shape.still_used = true;
    }
    return cached_shape.shape;
  }

  JPH::ShapeSettings::ShapeResult get_or_create_sphere(const CollisionShapeParams &params)
  {
    const std::optional<Bounds<float3>> bounds =
        params.geometry.compute_boundbox_without_instances(true);
    if (!bounds) {
      return {};
    }
    const float3 half_extent = math::max(math::abs(bounds->min), math::abs(bounds->max)) *
                               params.scale;
    const float radius = std::max({half_extent.x, half_extent.y, half_extent.z});
    return this->get_or_create_sphere(radius, params.density);
  }

  JPH::ShapeSettings::ShapeResult get_or_create_sphere(const float radius, const float density)
  {
    const SphereID sphere_id{radius, density};
    CachedShape &cached_shape = this->spheres.lookup_or_add_cb(sphere_id, [&]() {
      JPH::SphereShapeSettings sphere_shape_settings{radius};
      sphere_shape_settings.mDensity = density;
      return sphere_shape_settings.Create();
    });
    if (cached_shape.shape.IsValid()) {
      cached_shape.still_used = true;
    }
    return cached_shape.shape;
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
          JPH::ConvexHullShapeSettings hull_settings(points.data(), points.size(), 0.0f);
          hull_settings.mDensity = params.density;
          return hull_settings.Create();
        });
    if (cached_shape.shape.IsValid()) {
      cached_shape.still_used = true;
    }
    return cached_shape.shape;
  }
};

struct JoltState {
  bool is_initialized = false;
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

struct JoltBehaviors {
  Vector<ForceBehavior> forces;
  Vector<GravityBehavior> gravities;
  Vector<RigidBodyInstancesBehavior> rigid_bodies;
  Vector<SoftBodyMeshBehavior> soft_bodies;
};

static JoltBehaviors parse_behaviors(const Bundle &behaviors_bundle)
{
  JoltBehaviors behaviors;
  behaviors::foreach_behavior_in_bundle(
      behaviors_bundle,
      [&](const StringRef type, const Bundle &behavior_bundle, const Span<StringRef> path) {
        BehaviorParseErrors errors;
        if (type == ForceBehavior::type) {
          if (std::optional<ForceBehavior> force = ForceBehavior::parse(behavior_bundle, errors)) {
            behaviors.forces.append(std::move(*force));
            behaviors.forces.last().self_path = Bundle::combine_path(path);
          }
        }
        if (type == GravityBehavior::type) {
          if (std::optional<GravityBehavior> gravity = GravityBehavior::parse(behavior_bundle,
                                                                              errors)) {
            behaviors.gravities.append(std::move(*gravity));
            behaviors.gravities.last().self_path = Bundle::combine_path(path);
          }
        }
        else if (type == RigidBodyInstancesBehavior::type) {
          if (std::optional<RigidBodyInstancesBehavior> rigid_body =
                  RigidBodyInstancesBehavior::parse(behavior_bundle, errors))
          {
            behaviors.rigid_bodies.append(std::move(*rigid_body));
            behaviors.rigid_bodies.last().self_path = Bundle::combine_path(path);
          }
        }
        else if (type == SoftBodyMeshBehavior::type) {
          if (std::optional<SoftBodyMeshBehavior> soft_body = SoftBodyMeshBehavior::parse(
                  behavior_bundle, errors))
          {
            behaviors.soft_bodies.append(std::move(*soft_body));
            behaviors.soft_bodies.last().self_path = Bundle::combine_path(path);
          }
        }
      });
  return behaviors;
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

static void handle_rigid_bodies_behavior(JoltState &state,
                                         const RigidBodyInstancesBehavior &behavior,
                                         const bke::Instances &current_instances,
                                         Map<std::string, JoltRigidBodies> &r_rigid_bodies_by_path)
{
  JPH::BodyInterface &body_interface = state.system.GetBodyInterfaceNoLock();
  const int instances_num = current_instances.instances_num();
  const int references_num = current_instances.references_num();
  const Span<int> instance_ids = current_instances.almost_unique_ids();
  const Span<float4x4> transforms = current_instances.transforms();
  const Span<bke::InstanceReference> references = current_instances.references();
  const Span<int> handles = current_instances.reference_handles();

  bke::InstancesFieldContext field_context{current_instances};
  fn::FieldEvaluator field_evaluator{field_context, instances_num};
  field_evaluator.add(behavior.collision_shape_type);
  field_evaluator.add(behavior.motion_type);
  field_evaluator.add(behavior.friction);
  field_evaluator.add(behavior.bounciness);
  field_evaluator.add(behavior.density);
  field_evaluator.evaluate();
  const VArray<int> collision_shape_types = field_evaluator.get_evaluated<int>(0);
  const VArray<int> motion_types = field_evaluator.get_evaluated<int>(1);
  const VArray<float> frictions = field_evaluator.get_evaluated<float>(2);
  const VArray<float> bouncinesses = field_evaluator.get_evaluated<float>(3);
  const VArray<float> densities = field_evaluator.get_evaluated<float>(4);

  Array<GeometrySet> reference_geometry_sets(references_num);
  for (const int i : references.index_range()) {
    const bke::InstanceReference &reference = references[i];
    GeometrySet reference_geometry;
    reference.to_geometry_set(reference_geometry);
    reference_geometry_sets[i] = std::move(reference_geometry);
  }

  JoltRigidBodies *old_rigid_bodies = state.rigid_bodies_by_path.lookup_ptr(behavior.self_path);

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
    const GeometrySet &reference_geometry = reference_geometry_sets[reference_i];
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
        *collision_shape_type, collision_shape_params, state.collision_shape_cache);
    if (!collision_shape.IsValid()) {
      continue;
    }

    const float friction = frictions[instance_i];
    const float bounciness = bouncinesses[instance_i];

    std::optional<JoltRigidBody> rigid_body;
    if (old_rigid_bodies) {
      if (std::optional<JoltRigidBody> old_rigid_body = old_rigid_bodies->bodies_by_id.pop_try(
              instance_i))
      {
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

  r_rigid_bodies_by_path.add(behavior.self_path, std::move(rigid_bodies));
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

static void handle_soft_body_behavior(JoltState &state,
                                      const SoftBodyMeshBehavior &behavior,
                                      const Mesh &current_mesh,
                                      Map<std::string, JoltSoftBody> &r_soft_bodies_by_path)
{
  JPH::BodyInterface &body_interface = state.system.GetBodyInterfaceNoLock();

  JoltSoftBody soft_body;
  std::optional<JoltSoftBody> old_soft_body = state.soft_bodies_by_path.pop_try(
      behavior.self_path);
  if (old_soft_body) {
    const auto &motion_properties = *static_cast<const JPH::SoftBodyMotionProperties *>(
        old_soft_body->body->GetMotionProperties());
    if (motion_properties.GetVertices().size() == current_mesh.verts_num) {
      soft_body = std::move(*old_soft_body);
    }
  }
  if (!soft_body.body) {
    bke::MeshFieldContext field_context{current_mesh, bke::AttrDomain::Point};
    fn::FieldEvaluator field_evaluator{field_context, current_mesh.verts_num};
    field_evaluator.add(behavior.stretch_stiffness);
    field_evaluator.add(behavior.bend_stiffness);
    field_evaluator.evaluate();
    const VArray<float> stretch_stiffnesses = field_evaluator.get_evaluated<float>(0);
    const VArray<float> bend_stiffnesses = field_evaluator.get_evaluated<float>(1);

    JPH::Ref<JPH::SoftBodySharedSettings> shared_settings = new JPH::SoftBodySharedSettings();
    const Span<float3> positions = current_mesh.vert_positions();

    for (int i : positions.index_range()) {
      const float3 &position = positions[i];
      JPH::SoftBodySharedSettings::Vertex vertex;
      convert_vec3(position).StoreFloat3(&vertex.mPosition);
      shared_settings->mVertices.push_back(vertex);
    }

    const Span<int3> tris = current_mesh.corner_tris();
    const Span<int> corner_verts = current_mesh.corner_verts();
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
    if (!soft_body.body) {
      return;
    }
    body_interface.AddBody(soft_body.body->GetID(), JPH::EActivation::Activate);
  }

  BLI_assert(soft_body.body);
  r_soft_bodies_by_path.add(behavior.self_path, std::move(soft_body));
}

static GeometrySet apply_rigid_body_simulation(const RigidBodyInstancesBehavior &behavior,
                                               const JoltState &state)
{
  GeometrySet geometry = behavior.instances_geometry;
  bke::Instances *instances = geometry.get_instances_for_write();
  if (!instances) {
    return geometry;
  }

  const JoltRigidBodies *rigid_bodies = state.rigid_bodies_by_path.lookup_ptr(behavior.self_path);
  if (!rigid_bodies) {
    return geometry;
  }

  const int instances_num = instances->instances_num();
  const Span<int> instance_ids = instances->almost_unique_ids();
  MutableSpan<float4x4> transforms = instances->transforms_for_write();

  for (const int instance_i : IndexRange(instances_num)) {
    const int instance_id = instance_ids[instance_i];
    const JoltRigidBody *rigid_body = rigid_bodies->bodies_by_id.lookup_ptr(instance_id);
    if (!rigid_body) {
      continue;
    }
    const JPH::Body &jolt_body = *rigid_body->body;

    float4x4 &transform = transforms[instance_i];
    const JPH::Vec3 jolt_position = jolt_body.GetPosition();
    const JPH::Quat jolt_rotation = jolt_body.GetRotation();

    const float3 position = convert_vec3(jolt_position);
    const math::Quaternion rotation = convert_quat(jolt_rotation);

    /* Scale is not simulated to Jolt, so keep the scale of the original geometry. */
    const float3 scale = math::to_scale(transform);

    transform = math::from_loc_rot_scale<float4x4>(position, rotation, scale);
  }

  return geometry;
}

static GeometrySet apply_soft_body_simulation(const SoftBodyMeshBehavior &behavior,
                                              const JoltState &state)
{
  GeometrySet geometry = behavior.mesh_geometry;
  Mesh *mesh = geometry.get_mesh_for_write();
  if (!mesh) {
    return geometry;
  }
  const JoltSoftBody *soft_body = state.soft_bodies_by_path.lookup_ptr(behavior.self_path);
  if (!soft_body) {
    return geometry;
  }
  BLI_assert(soft_body->body->IsSoftBody());
  const auto &motion_properties = *static_cast<const JPH::SoftBodyMotionProperties *>(
      soft_body->body->GetMotionProperties());
  const JPH::Array<JPH::SoftBodyVertex> &vertices = motion_properties.GetVertices();
  if (mesh->verts_num != vertices.size()) {
    return geometry;
  }
  const JPH::Vec3 center = soft_body->body->GetPosition();
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const JPH::SoftBodyVertex &soft_body_vertex = vertices[i];
      positions[i] = convert_vec3(soft_body_vertex.mPosition + center);
    }
  });
  mesh->tag_positions_changed();
  return geometry;
}

static void update_gravity(const GeoNodeExecParams &params,
                           JoltState &state,
                           const JoltBehaviors &behaviors)
{
  if (behaviors.gravities.size() >= 2) {
    params.error_message_add(NodeWarningType::Warning, "There can't be multiple gravities");
    state.system.SetGravity(JPH::Vec3::sZero());
    return;
  }
  if (behaviors.gravities.size() == 1) {
    const GravityBehavior &behavior = behaviors.gravities[0];
    const float3 gravity = behavior.gravity;
    state.system.SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
    return;
  }

  const JPH::Vec3 default_gravity(0.0f, 0.0f, -9.81f);
  state.system.SetGravity(default_gravity);
}

static void apply_forces_on_rigid_bodies(JoltState &state,
                                         const JoltBehaviors &behaviors,
                                         const Span<GeometrySet> applied_rigid_bodies)
{
  JPH::BodyInterface &body_interface = state.system.GetBodyInterfaceNoLock();
  for (const int rigid_body_behavior_i : behaviors.rigid_bodies.index_range()) {
    const RigidBodyInstancesBehavior &rigid_body_behavior =
        behaviors.rigid_bodies[rigid_body_behavior_i];
    Vector<const ForceBehavior *> used_forces;
    for (const ForceBehavior &force_behavior : behaviors.forces) {
      if (behaviors::behavior_path_is_selected(
              force_behavior.self_path, force_behavior.filter, rigid_body_behavior.self_path))
      {
        used_forces.append(&force_behavior);
      }
    }
    if (used_forces.is_empty()) {
      continue;
    }
    JoltRigidBodies *rigid_bodies = state.rigid_bodies_by_path.lookup_ptr(
        rigid_body_behavior.self_path);
    if (!rigid_bodies) {
      continue;
    }
    const bke::Instances *instances = applied_rigid_bodies[rigid_body_behavior_i].get_instances();
    if (!instances) {
      continue;
    }
    /* Can also attempt to evaluate forces together but special care needs to be taken with the
     * selection. */
    Array<float3> force_sums(instances->instances_num(), float3(0.0f));
    for (const ForceBehavior *force_behavior : used_forces) {
      bke::InstancesFieldContext field_context{*instances};
      fn::FieldEvaluator field_evaluator{field_context, instances->instances_num()};
      field_evaluator.set_selection(force_behavior->selection);
      field_evaluator.add(force_behavior->force);
      field_evaluator.evaluate();
      const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
      const VArray<float3> force = field_evaluator.get_evaluated<float3>(0);
      mask.foreach_index([&](const int i) { force_sums[i] += force[i]; });
    }

    const Span<int> instance_ids = instances->almost_unique_ids();
    for (const int i : instance_ids.index_range()) {
      const int instance_id = instance_ids[i];
      const JoltRigidBody *rigid_body = rigid_bodies->bodies_by_id.lookup_ptr(instance_id);
      if (!rigid_body) {
        continue;
      }
      if (!rigid_body->body->IsDynamic()) {
        continue;
      }
      const float3 force = force_sums[i];
      body_interface.AddForce(rigid_body->body->GetID(), convert_vec3(force));
    }
  }
}

static void apply_forces(JoltState &state,
                         const JoltBehaviors &behaviors,
                         const Span<GeometrySet> applied_rigid_bodies,
                         const Span<GeometrySet> /*applied_soft_bodies*/)
{
  apply_forces_on_rigid_bodies(state, behaviors, applied_rigid_bodies);
  /* TODO: Forces on soft bodies. */
}

static void update_jolt_state_from_behaviors(const GeoNodeExecParams &params,
                                             JoltState &state,
                                             JoltBehaviors &behaviors)
{
  state.collision_shape_cache.reset_used();

  Array<GeometrySet> applied_rigid_bodies(behaviors.rigid_bodies.size());
  for (const int i : behaviors.rigid_bodies.index_range()) {
    const RigidBodyInstancesBehavior &rigid_body_behavior = behaviors.rigid_bodies[i];
    applied_rigid_bodies[i] = apply_rigid_body_simulation(rigid_body_behavior, state);
  }
  Array<GeometrySet> applied_soft_bodies(behaviors.soft_bodies.size());
  for (const int i : behaviors.soft_bodies.index_range()) {
    const SoftBodyMeshBehavior &soft_body_behavior = behaviors.soft_bodies[i];
    applied_soft_bodies[i] = apply_soft_body_simulation(soft_body_behavior, state);
  }

  JPH::BodyInterface &body_interface = state.system.GetBodyInterfaceNoLock();

  Map<std::string, JoltRigidBodies> new_rigid_bodies_by_path;
  for (const int i : behaviors.rigid_bodies.index_range()) {
    const GeometrySet &applied_rigid_body = applied_rigid_bodies[i];
    const bke::Instances *instances = applied_rigid_body.get_instances();
    if (!instances) {
      continue;
    }
    const RigidBodyInstancesBehavior &rigid_body_behavior = behaviors.rigid_bodies[i];
    handle_rigid_bodies_behavior(state, rigid_body_behavior, *instances, new_rigid_bodies_by_path);
  }
  Map<std::string, JoltSoftBody> new_soft_bodies_by_path;
  for (const int i : behaviors.soft_bodies.index_range()) {
    const GeometrySet &applied_soft_body = applied_soft_bodies[i];
    const Mesh *mesh = applied_soft_body.get_mesh();
    if (!mesh) {
      continue;
    }
    const SoftBodyMeshBehavior &soft_body_behavior = behaviors.soft_bodies[i];
    handle_soft_body_behavior(state, soft_body_behavior, *mesh, new_soft_bodies_by_path);
  }

  /* Remove old bodies. */
  Vector<JPH::BodyID> bodies_to_remove;
  for (JoltRigidBodies &rigid_bodies : state.rigid_bodies_by_path.values()) {
    for (JoltRigidBody &body : rigid_bodies.bodies_by_id.values()) {
      bodies_to_remove.append(body.body->GetID());
    }
  }
  for (JoltSoftBody &soft_body : state.soft_bodies_by_path.values()) {
    bodies_to_remove.append(soft_body.body->GetID());
  }
  body_interface.RemoveBodies(bodies_to_remove.data(), bodies_to_remove.size());
  body_interface.DestroyBodies(bodies_to_remove.data(), bodies_to_remove.size());

  state.rigid_bodies_by_path = std::move(new_rigid_bodies_by_path);
  state.soft_bodies_by_path = std::move(new_soft_bodies_by_path);
  state.collision_shape_cache.remove_unused();

  update_gravity(params, state, behaviors);
  apply_forces(state, behaviors, applied_rigid_bodies, applied_soft_bodies);
}

class JoltStateOwner : public BundleItemInternalValueMixin {
 public:
  mutable Mutex mutex;
  mutable JoltState state;

  void delete_self() override
  {
    MEM_delete(this);
  }

  StringRefNull type_name() const override
  {
    return TIP_("Jolt Physics State");
  }
};
using JoltStateOwnerPtr = ImplicitSharingPtr<JoltStateOwner>;

struct JoltStartupAndExit {
  JoltStartupAndExit()
  {
    initialize_jolt_allocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  }

  static void initialize_jolt_allocator()
  {
    constexpr const char *func = __func__;
    JPH::Allocate = [](size_t size) {
      /* Jolt requires 16-byte alignment when doing a normal allocation. */
      return MEM_mallocN_aligned(size, 16, func);
    };
    JPH::Reallocate = [](void *mem, size_t /*old_size*/, size_t new_size) {
      if (mem == nullptr) {
        return JPH::Allocate(new_size);
      }
      return MEM_reallocN_id(mem, new_size, func);
    };
    JPH::Free = [](void *mem) { MEM_freeN(mem); };
    JPH::AlignedAllocate = [](size_t size, size_t alignment) {
      return MEM_mallocN_aligned(size, alignment, func);
    };
    JPH::AlignedFree = [](void *mem) { MEM_freeN(mem); };
  }

  ~JoltStartupAndExit()
  {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
  }
};

static void ensure_initialize_jolt()
{
  static JoltStartupAndExit jolt_startup_and_exit;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr old_data_bundle = params.extract_input<BundlePtr>("Data");
  BundlePtr behavior_bundle = params.extract_input<BundlePtr>("Behavior");
  const float delta_time = params.extract_input<float>("Delta Time");
  const int sub_steps = params.extract_input<int>("Substeps");

  if (!behavior_bundle) {
    params.set_default_remaining_outputs();
    return;
  }

  ensure_initialize_jolt();

  int update_counter = 0;
  if (old_data_bundle) {
    update_counter = old_data_bundle->lookup<int>("_counter").value_or(0);
  }

  JoltStateOwnerPtr jolt_state_owner;
  if (old_data_bundle) {
    jolt_state_owner = old_data_bundle->lookup<JoltStateOwnerPtr>("_state").value_or(nullptr);
  }
  if (!jolt_state_owner) {
    jolt_state_owner = JoltStateOwnerPtr{MEM_new<JoltStateOwner>(__func__)};
  }

  if (!jolt_state_owner->mutex.try_lock()) {
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Jolt physics state cannot be used by multiple nodes"));
    params.set_default_remaining_outputs();
    return;
  }
  BLI_SCOPED_DEFER([&]() { jolt_state_owner->mutex.unlock(); });

  JoltState &state = jolt_state_owner->state;
  if (!state.is_initialized) {
    /* Defaults taken from Jolt's Hello World example, may need to be tweaked/dynamic over
     * time.*/
    const int max_bodies = 65536;
    const int num_body_mutexes = 0;
    const int max_body_pairs = 65536;
    const int max_contact_constraints = 10240;
    state.system.Init(max_bodies,
                      num_body_mutexes,
                      max_body_pairs,
                      max_contact_constraints,
                      state.broad_phase_layer_interface,
                      state.object_vs_broad_phase_layer_filter,
                      state.object_layer_pair_filter);
    state.system.SetBodyActivationListener(&state.body_activation_listener);
    state.system.SetContactListener(&state.contact_listener);
    state.job_system.emplace(JPH::cMaxPhysicsJobs);
    state.is_initialized = true;
  }
  JoltBehaviors behaviors = parse_behaviors(*behavior_bundle);

  /* The Jolt state can't easily be reset to an older state. So better just don't do simulation
   * in this case. */
  const bool is_resimulating = update_counter < state.update_counter;
  update_counter++;
  if (!is_resimulating) {
    update_jolt_state_from_behaviors(params, state, behaviors);

    {
      JPH::TempAllocatorImpl temp_allocator(10 * 1024 * 1024);
      const int collision_steps = sub_steps;
      state.system.Update(delta_time, collision_steps, &temp_allocator, &*state.job_system);
    }

    state.update_counter = update_counter;
  }

  BundlePtr new_data_bundle_ptr = Bundle::create();
  Bundle &new_data_bundle = const_cast<Bundle &>(*new_data_bundle_ptr);

  for (RigidBodyInstancesBehavior &rigid_bodies_behavior : behaviors.rigid_bodies) {
    GeometrySet applied_rigid_bodies = apply_rigid_body_simulation(rigid_bodies_behavior, state);
    new_data_bundle.add_path_override(rigid_bodies_behavior.self_path + "/Instances",
                                      std::move(applied_rigid_bodies));
  }
  for (SoftBodyMeshBehavior &soft_body_behavior : behaviors.soft_bodies) {
    GeometrySet applied_soft_body = apply_soft_body_simulation(soft_body_behavior, state);
    new_data_bundle.add_path_override(soft_body_behavior.self_path + "/Geometry",
                                      std::move(applied_soft_body));
  }

  new_data_bundle.add("_state", jolt_state_owner);
  new_data_bundle.add("_counter", update_counter);
  params.set_output("Data", std::move(new_data_bundle_ptr));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeJoltPhysicsSolver");
  ntype.ui_name = "Jolt Physics Solver";
  ntype.ui_description = "Simulate physics using the Jolt physics engine";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_jolt_physics_solver_cc
