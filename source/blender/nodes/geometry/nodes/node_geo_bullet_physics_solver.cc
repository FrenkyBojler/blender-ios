/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <LinearMath/btConvexHullComputer.h>
#include <fmt/format.h>

#include "BKE_curves.hh"
#include "BKE_instances.hh"

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"

#include "BLI_math_euler.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_struct_equality_utils.hh"

#include "node_geometry_util.hh"

#include <btBulletDynamicsCommon.h>

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"

#include "BLI_bounds.hh"

namespace blender::nodes::node_geo_bullet_physics_solver_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bundle>("Data");
  b.add_output<decl::Bundle>("Data").align_with_previous().field_on_all().structure_type(
      StructureType::Single);
  b.add_input<decl::Bundle>("Behavior");
  b.add_input<decl::Float>("Delta Time").min(0).hide_value();
  b.add_input<decl::Int>("Substeps").default_value(10).min(1);
  b.add_input<decl::Int>("Solver Steps").default_value(10).min(1);
}

static float4x4 btTransform_to_float4x4(const btTransform &transform)
{
  MatBase<btScalar, 4, 4> result;
  transform.getOpenGLMatrix(&result[0][0]);
  return float4x4(result);
}

// static btTransform float4x4_to_btTransform(const float4x4 &matrix)
// {
//   MatBase<btScalar, 4, 4> converted{matrix};
//   btTransform result;
//   result.setFromOpenGLMatrix(&converted[0][0]);
//   return result;
// }

struct MeshCollisionShapeData {
  Vector<btVector3> positions;
  Vector<int3> indices;
  std::unique_ptr<btTriangleIndexVertexArray> collision_mesh;
  std::unique_ptr<btBvhTriangleMeshShape> bvh_mesh;
};

struct CollisionShapeData {
  std::shared_ptr<btCollisionShape> shape;
  std::shared_ptr<MeshCollisionShapeData> collision_mesh;

  operator bool() const
  {
    return this->shape != nullptr;
  }
};

struct SingleRigidBody {
  CollisionShapeData collision_shape;
  std::unique_ptr<btDefaultMotionState> motion_state;
  std::unique_ptr<btRigidBody> body;
  float4x4 prev_kinematic_transform;

  float4x4 get_transform() const
  {
    btTransform result;
    this->motion_state->getWorldTransform(result);
    const btVector3 scale = this->body->getCollisionShape()->getLocalScaling();
    float4x4 result_transform = btTransform_to_float4x4(result);
    result_transform.x_axis() *= scale.x();
    result_transform.y_axis() *= scale.y();
    result_transform.z_axis() *= scale.z();
    return result_transform;
  }
};

struct RigidBodyInstances {
  GeometrySet geometry_set;
  Map<int, SingleRigidBody> rigid_body_by_id;
};

struct BulletState {
  bool is_initialized = false;
  int update_counter = 0;
  std::unique_ptr<btDefaultCollisionConfiguration> collision_configuration;
  std::unique_ptr<btCollisionDispatcher> collision_dispatcher;
  std::unique_ptr<btDbvtBroadphase> broadphase;
  std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
  std::unique_ptr<btDiscreteDynamicsWorld> dynamics_world;

  Map<std::string, RigidBodyInstances> rigid_body_instances_by_path;
  Vector<std::unique_ptr<btTypedConstraint>> constraints;
};

class BulletStateOwner : public BundleItemInternalValueMixin {
 public:
  mutable Mutex mutex;
  mutable BulletState state;

  void delete_self() override
  {
    MEM_delete(this);
  }

  StringRefNull type_name() const override
  {
    return TIP_("Bullet Physics State");
  }
};

using BulletStateOwnerPtr = ImplicitSharingPtr<BulletStateOwner>;

struct Force {
  std::string self_path;
  std::string filter;
  Field<float3> force_field;
};

struct DistanceConstraintGroup {
  std::string body_a;
  std::string body_b;
  Vector<int> ids_a;
  Vector<int> ids_b;
};

struct Behaviors {
  btVector3 gravity{};
  Map<std::string, RigidBodyInstances> new_rigid_body_instances_by_path;
  Vector<Force> forces;
  Vector<DistanceConstraintGroup> distance_constraint_groups;
};

struct ParseBehaviorParams {
  const Span<StringRef> path_elems;
  const Bundle &bundle;
  BulletState &state;
  Behaviors &behaviors;
  const float delta_time;

  std::string self_path() const
  {
    return Bundle::combine_path(this->path_elems);
  }
};

using BehaviorParseFn = std::function<void(ParseBehaviorParams &params)>;

static void parse_behavior__gravity(ParseBehaviorParams &params)
{
  const std::optional<float3> gravity = params.bundle.lookup<float3>("Gravity");
  if (!gravity) {
    return;
  }
  params.behaviors.gravity = btVector3(gravity->x, gravity->y, gravity->z);
}

static void parse_behavior__force(ParseBehaviorParams &params)
{
  const std::optional<Field<float3>> force_field = params.bundle.lookup<Field<float3>>(
      "Force Field");
  if (!force_field) {
    return;
  }
  params.behaviors.forces.append({params.self_path(),
                                  params.bundle.lookup<std::string>("Filter").value_or(""),
                                  *force_field});
}

enum class RigidBodyMode {
  Dynamic,
  Static,
  Animated,
};

enum class RigidBodyCollisionShape {
  Box,
  Sphere,
  ConvexHull,
  Mesh,
};

static std::optional<RigidBodyMode> parse_ridig_body_mode(const int mode)
{
  switch (mode) {
    case 0:
      return RigidBodyMode::Dynamic;
    case 1:
      return RigidBodyMode::Static;
    case 2:
      return RigidBodyMode::Animated;
    default:
      return std::nullopt;
  }
}

static std::optional<RigidBodyCollisionShape> parse_ridig_body_collision_shape(const int shape)
{
  switch (shape) {
    case 0:
      return RigidBodyCollisionShape::Box;
    case 1:
      return RigidBodyCollisionShape::Sphere;
    case 2:
      return RigidBodyCollisionShape::ConvexHull;
    case 3:
      return RigidBodyCollisionShape::Mesh;
    default:
      return std::nullopt;
  }
}

static std::optional<Bounds<float3>> gather_full_bounding_box(const GeometrySet &geometry)
{
  std::optional<Bounds<float3>> final_bounds;
  final_bounds = geometry.compute_boundbox_without_instances(true);
  const bke::Instances *instances = geometry.get_instances();
  if (!instances) {
    return final_bounds;
  }
  const int references_num = instances->references_num();
  const Span<bke::InstanceReference> references = instances->references();
  Array<std::optional<Bounds<float3>>> reference_bounds(references_num);
  for (const int i : IndexRange(references_num)) {
    const bke::InstanceReference &reference = references[i];
    bke::GeometrySet reference_geometry;
    reference.to_geometry_set(reference_geometry);
    reference_bounds[i] = gather_full_bounding_box(reference_geometry);
  }
  const int instances_num = instances->instances_num();
  const Span<int> handles = instances->reference_handles();
  const Span<float4x4> transforms = instances->transforms();
  for (const int i : IndexRange(instances_num)) {
    const int handle = handles[i];
    const std::optional<Bounds<float3>> &reference_bound = reference_bounds[handle];
    if (!reference_bound) {
      continue;
    }
    const Bounds<float3> transformed_bounds = bounds::transform_bounds(transforms[i],
                                                                       *reference_bound);
    final_bounds = bounds::merge(final_bounds, transformed_bounds);
  }
  return final_bounds;
}

static CollisionShapeData create_box_shape(const GeometrySet &geometry, const float margin)
{
  const std::optional<Bounds<float3>> bounds = gather_full_bounding_box(geometry);
  if (!bounds) {
    return {};
  }
  const float3 size = bounds->size();
  return {std::make_shared<btBoxShape>(btVector3(size.x, size.y, size.z) / 2.0f +
                                       btVector3(margin, margin, margin))};
}

static CollisionShapeData create_sphere_shape(const GeometrySet &geometry, const float margin)
{
  const std::optional<Bounds<float3>> bounds = gather_full_bounding_box(geometry);
  if (!bounds) {
    return {};
  }
  const float3 size = bounds->size();
  const float max_dimension = std::max({size.x, size.y, size.z});
  return {std::make_shared<btSphereShape>(max_dimension / 2.0f + margin)};
}

static CollisionShapeData create_convex_hull_shape(const GeometrySet &geometry, const float margin)
{
  // TODO: Take instance into account.
  Vector<float3> positions;
  if (const Mesh *mesh = geometry.get_mesh()) {
    positions.extend(mesh->vert_positions());
  }
  if (const Curves *curves = geometry.get_curves()) {
    positions.extend(curves->geometry.wrap().evaluated_positions());
  }
  btConvexHullComputer hull_computer;

  const Span<float> data = positions.as_span().cast<float>();
  hull_computer.compute(data.data(), sizeof(float3), positions.size(), 0.0f, 0.0f);
  if (hull_computer.vertices.size() == 0) {
    return {};
  }
  auto shape = std::make_shared<btConvexHullShape>(&hull_computer.vertices[0].getX(),
                                                   hull_computer.vertices.size());
  shape->setMargin(margin);
  return {shape};
}

static CollisionShapeData create_mesh_shape(const GeometrySet &geometry, const float margin)
{
  const Mesh *mesh = geometry.get_mesh();
  if (!mesh) {
    return {};
  }
  if (mesh->faces_num == 0) {
    return {};
  }

  auto mesh_shape_data = std::make_shared<MeshCollisionShapeData>();
  for (const float3 &pos : mesh->vert_positions()) {
    mesh_shape_data->positions.append(btVector3(pos.x, pos.y, pos.z));
  }
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<int3> tris = mesh->corner_tris();
  for (const int tri_i : tris.index_range()) {
    const int3 &tri = tris[tri_i];
    mesh_shape_data->indices.append(
        {corner_verts[tri[0]], corner_verts[tri[1]], corner_verts[tri[2]]});
  }

  mesh_shape_data->collision_mesh = std::make_unique<btTriangleIndexVertexArray>(
      mesh_shape_data->indices.size(),
      reinterpret_cast<int *>(mesh_shape_data->indices.data()),
      sizeof(int3),
      mesh_shape_data->positions.size(),
      reinterpret_cast<btScalar *>(mesh_shape_data->positions.data()),
      sizeof(float3));

  mesh_shape_data->bvh_mesh = std::make_unique<btBvhTriangleMeshShape>(
      mesh_shape_data->collision_mesh.get(), true, true);

  auto shape = std::make_shared<btScaledBvhTriangleMeshShape>(mesh_shape_data->bvh_mesh.get(),
                                                              btVector3(1.0f, 1.0f, 1.0f));
  shape->setMargin(margin);
  return {shape, mesh_shape_data};
}

static CollisionShapeData create_collision_shape(const RigidBodyCollisionShape shape,
                                                 const GeometrySet &geometry,
                                                 const float margin,
                                                 const RigidBodyMode mode)
{
  switch (shape) {
    case RigidBodyCollisionShape::Box:
      return create_box_shape(geometry, margin);
    case RigidBodyCollisionShape::Sphere:
      return create_sphere_shape(geometry, margin);
    case RigidBodyCollisionShape::ConvexHull:
      return create_convex_hull_shape(geometry, margin);
    case RigidBodyCollisionShape::Mesh:
      if (mode == RigidBodyMode::Dynamic) {
        /* Can't use the mesh shape for dynamic meshes currently. */
        return create_convex_hull_shape(geometry, margin);
      }
      else {
        /* TODO: This does not appear to be working yet. */
        return create_mesh_shape(geometry, margin);
      }
  }
  return {};
}

static void update_body_mode_if_necessary(BulletState &state,
                                          SingleRigidBody &body,
                                          const RigidBodyMode new_mode,
                                          const float new_mass,
                                          const float4x4 &new_transform,
                                          const float delta_time,
                                          const bool persist_velocity,
                                          const float3 &initial_velocity)
{
  btRigidBody &rbody = *body.body;
  const float old_mass = rbody.getMass();
  const int collision_flags = rbody.getCollisionFlags();
  const bool has_kinematic_flag = (collision_flags & btCollisionObject::CF_KINEMATIC_OBJECT) != 0;
  const bool has_static_flag = (collision_flags & btCollisionObject::CF_STATIC_OBJECT) != 0;
  switch (new_mode) {
    case RigidBodyMode::Dynamic: {
      if (old_mass > 0.0f && !has_kinematic_flag && !has_static_flag) {
        return;
      }
      state.dynamics_world->removeRigidBody(&rbody);
      btVector3 inertia(0, 0, 0);
      BLI_assert(new_mass > 0.0f);
      rbody.getCollisionShape()->calculateLocalInertia(new_mass, inertia);
      rbody.setMassProps(new_mass, inertia);
      rbody.setCollisionFlags(collision_flags & ~(btCollisionObject::CF_KINEMATIC_OBJECT |
                                                  btCollisionObject::CF_STATIC_OBJECT));
      rbody.setActivationState(ACTIVE_TAG);

      if (persist_velocity) {
        float3 old_pos;
        math::EulerXYZ old_rot;
        float3 old_scale;
        math::to_loc_rot_scale_safe<true>(
            body.prev_kinematic_transform, old_pos, old_rot, old_scale);

        float3 new_pos;
        math::EulerXYZ new_rot;
        float3 new_scale;
        math::to_loc_rot_scale_safe<true>(new_transform, new_pos, new_rot, new_scale);

        const float3 velocity = (new_pos - old_pos) / delta_time;
        rbody.setLinearVelocity(btVector3(velocity.x, velocity.y, velocity.z));
      }
      else {
        rbody.setLinearVelocity(
            btVector3(initial_velocity.x, initial_velocity.y, initial_velocity.z));
      }

      state.dynamics_world->addRigidBody(&rbody);
      break;
    }
    case RigidBodyMode::Static: {
      if (old_mass == 0.0f && !has_kinematic_flag && has_static_flag) {
        return;
      }
      state.dynamics_world->removeRigidBody(&rbody);
      rbody.setMassProps(0.0f, btVector3(0, 0, 0));
      rbody.setCollisionFlags((collision_flags & ~btCollisionObject::CF_KINEMATIC_OBJECT) |
                              btCollisionObject::CF_STATIC_OBJECT);
      state.dynamics_world->addRigidBody(&rbody);
      break;
    }
    case RigidBodyMode::Animated: {
      if (old_mass == 0.0f && has_kinematic_flag && !has_static_flag) {
        return;
      }
      state.dynamics_world->removeRigidBody(&rbody);
      rbody.setMassProps(0.0f, btVector3(0, 0, 0));
      rbody.setCollisionFlags((collision_flags & ~btCollisionObject::CF_STATIC_OBJECT) |
                              btCollisionObject::CF_KINEMATIC_OBJECT);
      state.dynamics_world->addRigidBody(&rbody);
      break;
    }
  }
}

struct CollisionShapeKey {
  int instance_id;
  RigidBodyCollisionShape shape;
  float margin;
  float3 scale;

  uint64_t hash() const
  {
    return get_default_hash(instance_id, shape, margin, scale);
  }

  BLI_STRUCT_EQUALITY_OPERATORS_4(CollisionShapeKey, instance_id, shape, margin, scale)
};

static void parse_behavior__rigid_body_instances(ParseBehaviorParams &params)
{
  std::optional<GeometrySet> geometry = params.bundle.lookup<GeometrySet>("Instances");
  if (!geometry) {
    return;
  }
  std::optional<Field<int>> mode_field = params.bundle.lookup<Field<int>>("Mode");
  if (!mode_field) {
    return;
  }
  std::optional<Field<int>> shape_field = params.bundle.lookup<Field<int>>("Collision Shape");
  if (!shape_field) {
    return;
  }
  std::optional<Field<float>> mass_field = params.bundle.lookup<Field<float>>("Mass");
  if (!mass_field) {
    mass_field = fn::make_constant_field<float>(1.0f);
  }
  std::optional<Field<float>> friction_field = params.bundle.lookup<Field<float>>("Friction");
  if (!friction_field) {
    friction_field = fn::make_constant_field<float>(0.0f);
  }
  std::optional<Field<float>> bounciness_field = params.bundle.lookup<Field<float>>("Bounciness");
  if (!bounciness_field) {
    bounciness_field = fn::make_constant_field<float>(0.0f);
  }
  std::optional<Field<float>> margin_field = params.bundle.lookup<Field<float>>("Margin");
  if (!margin_field) {
    margin_field = fn::make_constant_field<float>(0.0f);
  }
  std::optional<Field<float3>> initial_velocity_field = params.bundle.lookup<Field<float3>>(
      "Initial Velocity");
  if (!initial_velocity_field) {
    initial_velocity_field = fn::make_constant_field<float3>(float3(0.0f));
  }
  std::optional<Field<bool>> persist_velocity_field = params.bundle.lookup<Field<bool>>(
      "Persist Velocity");
  if (!persist_velocity_field) {
    persist_velocity_field = fn::make_constant_field<bool>(false);
  }
  bke::Instances *instances = geometry->get_instances_for_write();
  if (!instances) {
    return;
  }
  const std::string self_path = params.self_path();
  const int instances_num = instances->instances_num();
  const int references_num = instances->references_num();

  bke::InstancesFieldContext field_context{*instances};
  fn::FieldEvaluator field_evaluator{field_context, instances_num};
  field_evaluator.add(*mode_field);
  field_evaluator.add(*shape_field);
  field_evaluator.add(*mass_field);
  field_evaluator.add(*friction_field);
  field_evaluator.add(*bounciness_field);
  field_evaluator.add(*margin_field);
  field_evaluator.add(*initial_velocity_field);
  field_evaluator.add(*persist_velocity_field);
  field_evaluator.evaluate();
  const VArray<int> modes = field_evaluator.get_evaluated<int>(0);
  const VArray<int> shapes = field_evaluator.get_evaluated<int>(1);
  const VArray<float> masses = field_evaluator.get_evaluated<float>(2);
  const VArray<float> frictions = field_evaluator.get_evaluated<float>(3);
  const VArray<float> bouncinesses = field_evaluator.get_evaluated<float>(4);
  const VArray<float> margins = field_evaluator.get_evaluated<float>(5);
  const VArray<float3> initial_velocities = field_evaluator.get_evaluated<float3>(6);
  const VArray<bool> persist_velocities = field_evaluator.get_evaluated<bool>(7);

  const Span<int> instance_ids = instances->unique_ids();
  const Span<float4x4> transforms = instances->transforms();
  const Span<bke::InstanceReference> references = instances->references();
  const Span<int> handles = instances->reference_handles();

  Array<GeometrySet> reference_geometry_sets(references_num);
  Map<CollisionShapeKey, CollisionShapeData> collision_shapes;
  for (const int i : references.index_range()) {
    const bke::InstanceReference &reference = references[i];
    GeometrySet reference_geometry;
    reference.to_geometry_set(reference_geometry);
    reference_geometry_sets[i] = reference_geometry;
  }

  std::optional<RigidBodyInstances> old_rigid_body_instances =
      params.state.rigid_body_instances_by_path.pop_try(self_path);

  RigidBodyInstances rigid_body_instances;
  rigid_body_instances.geometry_set = *geometry;
  Map<int, SingleRigidBody> new_rigid_body_by_id;

  rigid_body_instances.rigid_body_by_id.reserve(instances_num);
  for (const int instance_i : IndexRange(instances_num)) {
    const int handle = handles[instance_i];
    const std::optional<RigidBodyMode> mode = parse_ridig_body_mode(modes[instance_i]);
    if (!mode) {
      continue;
    }
    const std::optional<RigidBodyCollisionShape> shape = parse_ridig_body_collision_shape(
        shapes[instance_i]);
    if (!shape) {
      continue;
    }
    const float4x4 &raw_transform = transforms[instance_i];
    float3 location;
    math::Quaternion rotation;
    float3 scale;
    math::to_loc_rot_scale_safe<true>(raw_transform, location, rotation, scale);

    btTransform bt_transform;
    bt_transform.setIdentity();
    bt_transform.setOrigin(btVector3(location.x, location.y, location.z));
    bt_transform.setRotation(btQuaternion(rotation.x, rotation.y, rotation.z, rotation.w));

    const float margin = std::max(margins[instance_i], 0.0f);

    const CollisionShapeData &collision_shape = collision_shapes.lookup_or_add_cb(
        {handle, *shape, margin, scale}, [&]() -> CollisionShapeData {
          if (CollisionShapeData new_shape = create_collision_shape(
                  *shape, reference_geometry_sets[handle], margin, *mode))
          {
            new_shape.shape->setLocalScaling(btVector3(scale.x, scale.y, scale.z));
            return new_shape;
          }
          return {};
        });
    if (!collision_shape) {
      continue;
    }

    const int instance_id = instance_ids[instance_i];

    std::optional<SingleRigidBody> old_body;
    if (old_rigid_body_instances) {
      old_body = old_rigid_body_instances->rigid_body_by_id.pop_try(instance_id);
    }

    float mass = masses[instance_i];
    switch (*mode) {
      case RigidBodyMode::Dynamic: {
        mass = std::max(mass, 0.0f);
        if (mass == 0.0f) {
          mass = 1.0f;
        }
        break;
      }
      case RigidBodyMode::Static:
      case RigidBodyMode::Animated: {
        mass = 0.0f;
        break;
      }
    }
    btVector3 inertia(0, 0, 0);
    collision_shape.shape->calculateLocalInertia(mass, inertia);

    const float3 initial_velocity = initial_velocities[instance_i];
    const bool persist_velocity = persist_velocities[instance_i];

    SingleRigidBody body;
    if (old_body) {
      body = std::move(*old_body);

      /* Update collision shape.*/
      if (ELEM(mode, RigidBodyMode::Dynamic, RigidBodyMode::Animated)) {
        params.state.dynamics_world->removeRigidBody(body.body.get());
        body.collision_shape = collision_shape;
        body.body->setCollisionShape(body.collision_shape.shape.get());
        params.state.dynamics_world->addRigidBody(body.body.get());
      }

      /* Update mode. */
      update_body_mode_if_necessary(params.state,
                                    body,
                                    *mode,
                                    mass,
                                    raw_transform,
                                    params.delta_time,
                                    persist_velocity,
                                    initial_velocity);

      /* Update transform. */
      if (mode == RigidBodyMode::Animated) {
        body.motion_state->setWorldTransform(bt_transform);
        body.body->setWorldTransform(bt_transform);
        body.prev_kinematic_transform = raw_transform;
      }
    }
    else {
      body.collision_shape = collision_shape;
      body.motion_state = std::make_unique<btDefaultMotionState>(bt_transform);
      btRigidBody::btRigidBodyConstructionInfo body_info(
          mass, &*body.motion_state, &*body.collision_shape.shape, inertia);
      body.body = std::make_unique<btRigidBody>(body_info);
      params.state.dynamics_world->addRigidBody(body.body.get());

      if (*mode == RigidBodyMode::Animated) {
        body.body->setCollisionFlags(body.body->getCollisionFlags() |
                                     btCollisionObject::CF_KINEMATIC_OBJECT);
      }
      if (*mode == RigidBodyMode::Dynamic) {
        body.body->setLinearVelocity(
            btVector3(initial_velocity.x, initial_velocity.y, initial_velocity.z));
      }
    }
    const float friction = frictions[instance_i];
    const float bounciness = bouncinesses[instance_i];
    if (body.body->getFriction() != friction) {
      body.body->setFriction(friction);
    }
    if (body.body->getRestitution() != bounciness) {
      body.body->setRestitution(bounciness);
    }
    body.body->setMassProps(mass, inertia);

    new_rigid_body_by_id.add_new(instance_id, std::move(body));
  }

  /* Remove unused rigid bodies from world. */
  if (old_rigid_body_instances) {
    for (const SingleRigidBody &old_rigid_body :
         old_rigid_body_instances->rigid_body_by_id.values())
    {
      params.state.dynamics_world->removeRigidBody(old_rigid_body.body.get());
    }
  }

  rigid_body_instances.rigid_body_by_id = std::move(new_rigid_body_by_id);
  params.behaviors.new_rigid_body_instances_by_path.add(self_path,
                                                        std::move(rigid_body_instances));
}

static void parse_behavior__rigid_body_constraints(ParseBehaviorParams &params)
{
  const std::optional<std::string> body_a = params.bundle.lookup<std::string>("Body A");
  const std::optional<std::string> body_b = params.bundle.lookup<std::string>("Body B");
  const ListPtr ids_a = params.bundle.lookup<ListPtr>("IDs A").value_or(nullptr);
  const ListPtr ids_b = params.bundle.lookup<ListPtr>("IDs B").value_or(nullptr);

  if (!body_a || !body_b || !ids_a || !ids_b) {
    return;
  }
  if (ids_a->size() != ids_b->size()) {
    return;
  }
  if (!ids_a->cpp_type().is<int>() || !ids_b->cpp_type().is<int>()) {
    return;
  }

  DistanceConstraintGroup group;
  group.body_a = *body_a;
  group.body_b = *body_b;
  group.ids_a.extend(VArraySpan<int>(ids_a->varray<int>()));
  group.ids_b.extend(VArraySpan<int>(ids_b->varray<int>()));
  params.behaviors.distance_constraint_groups.append(std::move(group));
}

static Map<std::string, BehaviorParseFn> build_behavior_parsers()
{
  Map<std::string, BehaviorParseFn> behavior_parsers;
  behavior_parsers.add_new("Gravity", parse_behavior__gravity);
  behavior_parsers.add_new("Force", parse_behavior__force);
  behavior_parsers.add_new("Rigid Body Instances", parse_behavior__rigid_body_instances);
  behavior_parsers.add_new("Rigid Body Distance Constraint",
                           parse_behavior__rigid_body_constraints);
  return behavior_parsers;
}

static void update_state_from_behaviors(BulletState &state,
                                        const Bundle &behavior_bundle,
                                        const float delta_time,
                                        Behaviors &r_behaviors)
{
  /* Clear all constraints for now. */
  while (state.dynamics_world->getNumConstraints() > 0) {
    state.dynamics_world->removeConstraint(state.dynamics_world->getConstraint(0));
  }
  state.constraints.clear();

  nested_bundle_foreach(behavior_bundle, [&](HandleNestedBundleParams &params) {
    ParseBehaviorParams my_params{params.path, params.bundle, state, r_behaviors, delta_time};
    if (const auto *behavior_parse = build_behavior_parsers().lookup_ptr(params.type)) {
      (*behavior_parse)(my_params);
    }
  });

  state.dynamics_world->setGravity(r_behaviors.gravity);

  /* Remove now unused rigid bodies. */
  for (const RigidBodyInstances &rigid_body_instances :
       state.rigid_body_instances_by_path.values())
  {
    for (const SingleRigidBody &single_rigid_body : rigid_body_instances.rigid_body_by_id.values())
    {
      state.dynamics_world->removeRigidBody(single_rigid_body.body.get());
    }
  }

  state.rigid_body_instances_by_path = std::move(r_behaviors.new_rigid_body_instances_by_path);

  for (const DistanceConstraintGroup &group : r_behaviors.distance_constraint_groups) {
    RigidBodyInstances *bodies_a = state.rigid_body_instances_by_path.lookup_ptr(group.body_a);
    RigidBodyInstances *bodies_b = state.rigid_body_instances_by_path.lookup_ptr(group.body_b);
    if (!bodies_a || !bodies_b) {
      continue;
    }
    const int constraints_num = group.ids_a.size();
    for (const int i : IndexRange(constraints_num)) {
      const int id_a = group.ids_a[i];
      const int id_b = group.ids_b[i];
      SingleRigidBody *body_a = bodies_a->rigid_body_by_id.lookup_ptr(id_a);
      SingleRigidBody *body_b = bodies_b->rigid_body_by_id.lookup_ptr(id_b);
      if (!body_a || !body_b) {
        continue;
      }

      btTransform object_a_to_world = body_a->body->getWorldTransform();
      btTransform object_b_to_world = body_b->body->getWorldTransform();
      btTransform world_to_object_b = object_b_to_world.inverse();
      btTransform object_a_to_object_b = world_to_object_b * object_a_to_world;

      btVector3 pivot_a{0, 0, 0};
      btVector3 pivot_b = object_a_to_object_b * pivot_a;

      auto constraint = std::make_unique<btPoint2PointConstraint>(
          *body_a->body, *body_b->body, pivot_a, pivot_b);
      state.dynamics_world->addConstraint(constraint.get(), true);
      state.constraints.append(std::move(constraint));
    }
  }
}

static void write_simulated_data_to_geometry_sets(BulletState &state)
{
  for (RigidBodyInstances &rigid_body_instances : state.rigid_body_instances_by_path.values()) {
    bke::Instances *instances = rigid_body_instances.geometry_set.get_instances_for_write();
    if (!instances) {
      continue;
    }
    const int instances_num = instances->instances_num();
    const Span<int> instance_ids = instances->unique_ids();
    const MutableSpan<float4x4> transforms = instances->transforms_for_write();
    for (const int instance_i : IndexRange(instances_num)) {
      const int instance_id = instance_ids[instance_i];
      const SingleRigidBody *body = rigid_body_instances.rigid_body_by_id.lookup_ptr(instance_id);
      if (!body) {
        continue;
      }
      transforms[instance_i] = body->get_transform();
    }
  }
}

static void apply_forces(BulletState &state, const Behaviors &behaviors)
{
  write_simulated_data_to_geometry_sets(state);
  for (auto &&item : state.rigid_body_instances_by_path.items()) {
    const StringRef instances_path = item.key;
    RigidBodyInstances &rigid_body_instances = item.value;
    const bke::Instances *instances = rigid_body_instances.geometry_set.get_instances();
    if (!instances) {
      continue;
    }

    Vector<const Force *> filtered_forces;
    for (const Force &force : behaviors.forces) {
      if (nested_bundle_path_is_selected(force.self_path, force.filter, instances_path)) {
        filtered_forces.append(&force);
      }
    }

    const int instances_num = instances->instances_num();
    const Span<int> instance_ids = instances->unique_ids();
    bke::InstancesFieldContext field_context{*instances};
    fn::FieldEvaluator field_evaluator{field_context, instances->instances_num()};
    for (const Force *force : filtered_forces) {
      field_evaluator.add(force->force_field);
    }
    field_evaluator.evaluate();
    Array<float3> force_sum(instances_num, float3(0.0f));
    for (const int force_i : filtered_forces.index_range()) {
      const VArray<float3> force = field_evaluator.get_evaluated<float3>(force_i);
      for (const int i : IndexRange(instances_num)) {
        force_sum[i] += force[i];
      }
    }
    for (SingleRigidBody &body : rigid_body_instances.rigid_body_by_id.values()) {
      body.body->clearForces();
    }
    for (const int instance_i : IndexRange(instances_num)) {
      const int instance_id = instance_ids[instance_i];
      const float3 &force = force_sum[instance_i];
      if (math::is_zero(force)) {
        continue;
      }
      if (SingleRigidBody *body = rigid_body_instances.rigid_body_by_id.lookup_ptr(instance_id)) {
        body->body->applyCentralForce(btVector3(force.x, force.y, force.z));
        body->body->activate();
      }
    }
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr old_data_bundle = params.extract_input<BundlePtr>("Data");
  BundlePtr behaviors_bundle = params.extract_input<BundlePtr>("Behavior");
  const float delta_time = params.extract_input<float>("Delta Time");
  const int sub_steps = std::max(params.extract_input<int>("Substeps"), 1);
  const int solver_steps = std::max(params.extract_input<int>("Solver Steps"), 1);

  if (!behaviors_bundle) {
    params.set_default_remaining_outputs();
    return;
  }

  int update_counter = 0;
  if (old_data_bundle) {
    update_counter = old_data_bundle->lookup<int>("_counter").value_or(0);
  }

  auto bullet_state_owner = BulletStateOwnerPtr{};
  if (old_data_bundle) {
    bullet_state_owner = old_data_bundle->lookup<BulletStateOwnerPtr>("_state").value_or(nullptr);
  }
  if (!bullet_state_owner) {
    bullet_state_owner = BulletStateOwnerPtr{MEM_new<BulletStateOwner>(__func__)};
  }

  if (!bullet_state_owner->mutex.try_lock()) {
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Bullet physics state cannot be used by multiple nodes"));
    params.set_default_remaining_outputs();
    return;
  }
  BLI_SCOPED_DEFER([&]() { bullet_state_owner->mutex.unlock(); });

  BulletState &state = bullet_state_owner->state;
  if (!state.is_initialized) {
    state.collision_configuration = std::make_unique<btDefaultCollisionConfiguration>();
    state.collision_dispatcher = std::make_unique<btCollisionDispatcher>(
        state.collision_configuration.get());
    state.broadphase = std::make_unique<btDbvtBroadphase>();
    state.solver = std::make_unique<btSequentialImpulseConstraintSolver>();
    state.dynamics_world = std::make_unique<btDiscreteDynamicsWorld>(
        state.collision_dispatcher.get(),
        state.broadphase.get(),
        state.solver.get(),
        state.collision_configuration.get());
    state.is_initialized = true;
  }

  /* The Bullet state can't easily be reset to an older state. So better just don't do simulation
   * in this case. */
  const bool is_resimulating = update_counter < state.update_counter;
  update_counter++;
  if (!is_resimulating) {
    Behaviors behaviors;
    update_state_from_behaviors(state, *behaviors_bundle, delta_time, behaviors);

    apply_forces(state, behaviors);

    const float time_per_step = delta_time / sub_steps;
    state.dynamics_world->getSolverInfo().m_numIterations = solver_steps;
    for ([[maybe_unused]] const int i : IndexRange(sub_steps)) {
      state.dynamics_world->stepSimulation(time_per_step, 0, time_per_step);
    }
    state.update_counter = update_counter;
  }
  write_simulated_data_to_geometry_sets(state);

  BundlePtr new_data_bundle_ptr = Bundle::create();
  Bundle &new_data_bundle = const_cast<Bundle &>(*new_data_bundle_ptr);
  new_data_bundle.add("_state", bullet_state_owner);
  new_data_bundle.add("_counter", update_counter);

  for (const auto &item : state.rigid_body_instances_by_path.items()) {
    const StringRef self_path = item.key;
    const RigidBodyInstances &rigid_body_instances = item.value;
    new_data_bundle.add_path(self_path + "/Instances", rigid_body_instances.geometry_set);
  }

  params.set_output("Data", std::move(new_data_bundle_ptr));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeBulletPhysicsSolver");
  ntype.ui_name = "Bullet Physics Solver";
  ntype.ui_description = "Simulate physics using the Bullet physics engine";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_bullet_physics_solver_cc
