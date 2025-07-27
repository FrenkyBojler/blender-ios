/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "DNA_mesh_types.h"

#include "node_geometry_util.hh"

#include <btBulletDynamicsCommon.h>

#include "NOD_geometry_nodes_behaviors_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"

namespace blender::nodes::node_geo_bullet_physics_solver_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bundle>("Data");
  b.add_output<decl::Bundle>("Data").align_with_previous();
  b.add_input<decl::Bundle>("Behavior");
  b.add_input<decl::Float>("Delta Time").min(0).hide_value();
  b.add_input<decl::Int>("Substeps").default_value(0).min(0);
}

static float4x4 btTransform_to_float4x4(const btTransform &transform)
{
  MatBase<btScalar, 4, 4> result;
  transform.getOpenGLMatrix(&result[0][0]);
  return float4x4(result);
}

static btTransform float4x4_to_btTransform(const float4x4 &matrix)
{
  MatBase<btScalar, 4, 4> converted{matrix};
  btTransform result;
  result.setFromOpenGLMatrix(&converted[0][0]);
  return result;
}

struct SingleRigidBody {
  std::unique_ptr<btCollisionShape> shape;
  std::unique_ptr<btDefaultMotionState> motion_state;
  std::unique_ptr<btRigidBody> body;

  float4x4 get_transform() const
  {
    btTransform result;
    motion_state->getWorldTransform(result);
    return btTransform_to_float4x4(result);
  }
};

struct BulletState {
  bool is_initialized = false;
  int update_counter = 0;
  std::unique_ptr<btDefaultCollisionConfiguration> collision_configuration;
  std::unique_ptr<btCollisionDispatcher> collision_dispatcher;
  std::unique_ptr<btDbvtBroadphase> broadphase;
  std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
  std::unique_ptr<btDiscreteDynamicsWorld> dynamics_world;

  Map<std::string, SingleRigidBody> single_rigid_bodies;
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

struct Behaviors {
  btVector3 gravity{};
  Map<std::string, SingleRigidBody> new_single_rigid_bodies;
};

struct ParseBehaviorParams {
  const Span<StringRef> path_elems;
  const Bundle &bundle;
  BulletState &state;
  Behaviors &behaviors;

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

enum class RigidBodyMode {
  Dynamic,
  Static,
  Animated,
};

static std::optional<RigidBodyMode> parse_ridig_body_mode(const Bundle &bundle)
{
  const std::optional<int> mode = bundle.lookup<int>("Mode");
  if (!mode) {
    return std::nullopt;
  }
  switch (*mode) {
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

static void parse_behavior__single_rigid_body(ParseBehaviorParams &params)
{
  std::optional<GeometrySet> geometry = params.bundle.lookup<GeometrySet>("Mesh");
  if (!geometry) {
    return;
  }
  std::optional<float4x4> transform = params.bundle.lookup<float4x4>("Transform");
  if (!transform) {
    return;
  }
  const float friction = params.bundle.lookup<float>("Friction").value_or(0.0f);
  const float bounciness = params.bundle.lookup<float>("Bounciness").value_or(0.0f);
  float mass = params.bundle.lookup<float>("Mass").value_or(0.0f);
  std::string self_path = params.self_path();
  const std::optional<RigidBodyMode> mode = parse_ridig_body_mode(params.bundle);
  if (!mode) {
    return;
  }

  if (mode != RigidBodyMode::Dynamic) {
    /* This makes the object non-dynamic in Bullet. */
    mass = 0.0f;
  }

  std::optional<SingleRigidBody> old_rigid_body = params.state.single_rigid_bodies.pop_try(
      self_path);
  SingleRigidBody rigid_body;
  if (old_rigid_body) {
    rigid_body = std::move(*old_rigid_body);
    switch (*mode) {
      case RigidBodyMode::Dynamic: {
        break;
      }
      case RigidBodyMode::Static:
      case RigidBodyMode::Animated: {
        rigid_body.motion_state->setWorldTransform(float4x4_to_btTransform(*transform));
        break;
      }
    }
    if (rigid_body.body->getFriction() != friction) {
      rigid_body.body->setFriction(friction);
    }
    if (rigid_body.body->getRestitution() != bounciness) {
      rigid_body.body->setRestitution(bounciness);
    }
  }
  else {
    const Mesh *mesh = geometry->get_mesh();
    if (!mesh) {
      return;
    }
    std::optional<Bounds<float3>> bounds = mesh->bounds_min_max();
    if (!bounds) {
      return;
    }
    const float3 size = bounds->size();

    rigid_body.shape = std::make_unique<btBoxShape>(btVector3(size.x, size.y, size.z) / 2.0f);
    rigid_body.motion_state = std::make_unique<btDefaultMotionState>(
        float4x4_to_btTransform(*transform));

    btVector3 inertia(0, 0, 0);
    switch (*mode) {
      case RigidBodyMode::Dynamic: {
        rigid_body.shape->calculateLocalInertia(mass, inertia);
        break;
      }
      case RigidBodyMode::Static:
      case RigidBodyMode::Animated: {
        break;
      }
    }
    btRigidBody::btRigidBodyConstructionInfo body_info(
        mass, &*rigid_body.motion_state, &*rigid_body.shape, inertia);
    rigid_body.body = std::make_unique<btRigidBody>(body_info);
    params.state.dynamics_world->addRigidBody(&*rigid_body.body);
    rigid_body.body->setFriction(friction);
    rigid_body.body->setRestitution(bounciness);
  }
  params.behaviors.new_single_rigid_bodies.add(self_path, std::move(rigid_body));
}

static Map<std::string, BehaviorParseFn> build_behavior_parsers()
{
  Map<std::string, BehaviorParseFn> behavior_parses;
  behavior_parses.add_new("Gravity", parse_behavior__gravity);
  behavior_parses.add_new("Single Rigid Body", parse_behavior__single_rigid_body);
  return behavior_parses;
}

static void update_state_from_behaviors(BulletState &state, const Bundle &behavior_bundle)
{
  Behaviors behaviors;
  foreach_behavior_in_bundle(
      behavior_bundle,
      [&](const StringRef type, const Bundle &behavior_bundle, const Span<StringRef> path) {
        ParseBehaviorParams params{path, behavior_bundle, state, behaviors};
        if (const auto *behavior_parse = build_behavior_parsers().lookup_ptr(type)) {
          (*behavior_parse)(params);
        }
      });

  state.dynamics_world->setGravity(behaviors.gravity);

  /* Remove now unused rigid bodies. */
  for (const SingleRigidBody &single_rigid_body : state.single_rigid_bodies.values()) {
    state.dynamics_world->removeRigidBody(single_rigid_body.body.get());
  }
  state.single_rigid_bodies = std::move(behaviors.new_single_rigid_bodies);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr old_data_bundle = params.extract_input<BundlePtr>("Data");
  BundlePtr behaviors_bundle = params.extract_input<BundlePtr>("Behavior");
  const float delta_time = params.extract_input<float>("Delta Time");
  const int substeps = params.extract_input<int>("Substeps");

  if (!behaviors_bundle) {
    params.set_default_remaining_outputs();
    return;
  }

  int update_counter = 0;
  if (old_data_bundle) {
    update_counter = old_data_bundle->lookup<int>("Update Counter").value_or(0);
  }

  auto bullet_state_owner = BulletStateOwnerPtr{};
  if (old_data_bundle) {
    bullet_state_owner =
        old_data_bundle->lookup<BulletStateOwnerPtr>("Bullet State").value_or(nullptr);
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

  update_state_from_behaviors(state, *behaviors_bundle);

  /* The Bullet state can't easily be reset to an older state. So better just don't do simulation
   * in this case. */
  const bool is_resimulating = update_counter < state.update_counter;
  update_counter++;
  if (!is_resimulating) {
    state.dynamics_world->stepSimulation(delta_time, substeps);
    state.update_counter = update_counter;
  }

  BundlePtr new_data_bundle_ptr = Bundle::create();
  Bundle &new_data_bundle = const_cast<Bundle &>(*new_data_bundle_ptr);
  new_data_bundle.add("Bullet State", bullet_state_owner);
  new_data_bundle.add("Update Counter", update_counter);

  for (const auto &item : state.single_rigid_bodies.items()) {
    const StringRef self_path = item.key;
    const SingleRigidBody &single_rigid_body = item.value;
    new_data_bundle.add_path(self_path + "/Transform", single_rigid_body.get_transform());
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
