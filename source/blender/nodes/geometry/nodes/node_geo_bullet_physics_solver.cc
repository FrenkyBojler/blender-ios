/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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

struct SingleRigidBody {
  std::unique_ptr<btCollisionShape> shape;
  std::unique_ptr<btDefaultMotionState> motion_state;
  std::unique_ptr<btRigidBody> body;
};

struct BulletState {
  bool is_initialized = false;
  std::unique_ptr<btDefaultCollisionConfiguration> collision_configuration;
  std::unique_ptr<btCollisionDispatcher> collision_dispatcher;
  std::unique_ptr<btDbvtBroadphase> broadphase;
  std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
  std::unique_ptr<btDiscreteDynamicsWorld> dynamics_world;

  Map<std::string, SingleRigidBody> single_rigid_bodies;

  std::unique_ptr<btCollisionShape> my_box_shape;
  std::unique_ptr<btDefaultMotionState> my_box_motion_state;
  std::unique_ptr<btRigidBody> my_box;

  std::unique_ptr<btCollisionShape> my_plane_shape;
  std::unique_ptr<btDefaultMotionState> my_plane_motion_state;
  std::unique_ptr<btRigidBody> my_plane;
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

static float4x4 btTransform_to_float4x4(const btTransform &transform)
{
  MatBase<btScalar, 4, 4> result;
  transform.getOpenGLMatrix(&result[0][0]);
  return float4x4(result);
}

struct Behaviors {
  btVector3 gravity{};
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

static Map<std::string, BehaviorParseFn> build_behavior_parses()
{
  Map<std::string, BehaviorParseFn> behavior_parses;
  behavior_parses.add_new("Gravity", parse_behavior__gravity);
  return behavior_parses;
}

static void update_state_from_behaviors(BulletState &state, const Bundle &behavior_bundle)
{
  Behaviors behaviors;
  foreach_behavior_in_bundle(
      behavior_bundle,
      [&](const StringRef type, const Bundle &behavior_bundle, const Span<StringRef> path) {
        ParseBehaviorParams params{path, behavior_bundle, state, behaviors};
        if (const auto *behavior_parse = build_behavior_parses().lookup_ptr(type)) {
          (*behavior_parse)(params);
        }
      });

  state.dynamics_world->setGravity(behaviors.gravity);
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

    /* Create a box. */
    {
      state.my_box_shape = std::make_unique<btBoxShape>(btVector3(1, 1, 1));
      btScalar mass = 1.0f;
      btVector3 inertia(0, 0, 0);
      state.my_box_shape->calculateLocalInertia(mass, inertia);
      state.my_box_motion_state = std::make_unique<btDefaultMotionState>(
          btTransform(btQuaternion(1, 2, 3), btVector3(0, 0, 10)));
      btRigidBody::btRigidBodyConstructionInfo my_box_info(
          mass, &*state.my_box_motion_state, &*state.my_box_shape, inertia);
      state.my_box = std::make_unique<btRigidBody>(my_box_info);
      state.dynamics_world->addRigidBody(&*state.my_box);
    }

    /* Create a plane. */
    {
      state.my_plane_shape = std::make_unique<btBoxShape>(btVector3(10.0f, 10.0f, 0.1f));
      /* Setting the mass to 0 makes the plane static. */
      const btScalar mass = 0.0f;
      state.my_plane_motion_state = std::make_unique<btDefaultMotionState>(
          btTransform(btQuaternion(0, 0, 0, 1), btVector3(0, 0, 0)));
      btRigidBody::btRigidBodyConstructionInfo my_plane_info(
          mass, &*state.my_plane_motion_state, &*state.my_plane_shape, btVector3(0, 0, 0));
      state.my_plane = std::make_unique<btRigidBody>(my_plane_info);
      state.dynamics_world->addRigidBody(&*state.my_plane);
    }
  }

  update_state_from_behaviors(state, *behaviors_bundle);

  state.dynamics_world->stepSimulation(delta_time, substeps);

  BundlePtr new_data_bundle_ptr = Bundle::create();
  Bundle &new_data_bundle = const_cast<Bundle &>(*new_data_bundle_ptr);
  new_data_bundle.add("Bullet State", bullet_state_owner);

  btTransform transform;
  state.my_box->getMotionState()->getWorldTransform(transform);
  float4x4 matrix = btTransform_to_float4x4(transform);
  new_data_bundle.add("Transform", matrix);

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
