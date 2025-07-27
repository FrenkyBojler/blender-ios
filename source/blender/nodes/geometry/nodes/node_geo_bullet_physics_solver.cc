/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "BKE_instances.hh"
#include "DNA_mesh_types.h"

#include "node_geometry_util.hh"

#include <btBulletDynamicsCommon.h>

#include "NOD_geometry_nodes_behaviors_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"

#include "BLI_bounds.hh"

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
  Map<std::string, RigidBodyInstances> new_rigid_body_instances_by_path;
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
  field_evaluator.add(*mass_field);
  field_evaluator.add(*friction_field);
  field_evaluator.add(*bounciness_field);
  field_evaluator.evaluate();
  const VArray<int> modes = field_evaluator.get_evaluated<int>(0);
  const VArray<float> masses = field_evaluator.get_evaluated<float>(1);
  const VArray<float> frictions = field_evaluator.get_evaluated<float>(2);
  const VArray<float> bouncinesses = field_evaluator.get_evaluated<float>(3);

  const Span<int> instance_ids = instances->almost_unique_ids();
  const Span<float4x4> transforms = instances->transforms();
  const Span<bke::InstanceReference> references = instances->references();
  const Span<int> handles = instances->reference_handles();

  Array<GeometrySet> reference_geometry_sets(references_num);
  Array<std::optional<Bounds<float3>>> reference_bounds(references_num);
  for (const int i : references.index_range()) {
    const bke::InstanceReference &reference = references[i];
    GeometrySet reference_geometry;
    reference.to_geometry_set(reference_geometry);
    reference_geometry_sets[i] = reference_geometry;
    reference_bounds[i] = reference_geometry.compute_boundbox_without_instances();
  }

  std::optional<RigidBodyInstances> old_rigid_body_instances =
      params.state.rigid_body_instances_by_path.pop_try(self_path);

  RigidBodyInstances rigid_body_instances;
  rigid_body_instances.geometry_set = *geometry;
  Map<int, SingleRigidBody> new_rigid_body_by_id;

  rigid_body_instances.rigid_body_by_id.reserve(instances_num);
  for (const int instance_i : IndexRange(instances_num)) {
    const int handle = handles[instance_i];
    const std::optional<Bounds<float3>> &bounds = reference_bounds[handle];
    if (!bounds) {
      continue;
    }
    std::optional<RigidBodyMode> mode = parse_ridig_body_mode(modes[instance_i]);
    if (!mode) {
      continue;
    }
    const int instance_id = instance_ids[instance_i];
    const float4x4 &transform = transforms[instance_i];
    const float3 size = bounds->size();
    std::optional<SingleRigidBody> old_body;
    if (old_rigid_body_instances) {
      old_body = old_rigid_body_instances->rigid_body_by_id.pop_try(instance_id);
    }

    SingleRigidBody body;
    if (old_body) {
      body = std::move(*old_body);
      switch (*mode) {
        case RigidBodyMode::Dynamic: {
          break;
        }
        case RigidBodyMode::Static: {
          break;
        }
        case RigidBodyMode::Animated: {
          const btTransform bt_transform = float4x4_to_btTransform(transform);
          body.motion_state->setWorldTransform(bt_transform);
          body.body->setWorldTransform(bt_transform);
          break;
        }
      }
    }
    else {
      body.shape = std::make_unique<btBoxShape>(btVector3(size.x, size.y, size.z) / 2.0f);
      body.motion_state = std::make_unique<btDefaultMotionState>(
          float4x4_to_btTransform(transform));
      btVector3 inertia(0, 0, 0);
      float mass = std::max(masses[instance_i], 0.0f);
      switch (*mode) {
        case RigidBodyMode::Dynamic: {
          body.shape->calculateLocalInertia(masses[instance_i], inertia);
          break;
        }
        case RigidBodyMode::Static:
        case RigidBodyMode::Animated: {
          mass = 0.0f;
          break;
        }
      }
      btRigidBody::btRigidBodyConstructionInfo body_info(
          mass, &*body.motion_state, &*body.shape, inertia);
      body.body = std::make_unique<btRigidBody>(body_info);
      params.state.dynamics_world->addRigidBody(body.body.get());

      if (*mode == RigidBodyMode::Animated) {
        body.body->setCollisionFlags(body.body->getCollisionFlags() |
                                     btCollisionObject::CF_KINEMATIC_OBJECT);
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

static Map<std::string, BehaviorParseFn> build_behavior_parsers()
{
  Map<std::string, BehaviorParseFn> behavior_parsers;
  behavior_parsers.add_new("Gravity", parse_behavior__gravity);
  behavior_parsers.add_new("Rigid Body Instances", parse_behavior__rigid_body_instances);
  return behavior_parsers;
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
  for (const RigidBodyInstances &rigid_body_instances :
       state.rigid_body_instances_by_path.values())
  {
    for (const SingleRigidBody &single_rigid_body : rigid_body_instances.rigid_body_by_id.values())
    {
      state.dynamics_world->removeRigidBody(single_rigid_body.body.get());
    }
  }

  state.rigid_body_instances_by_path = std::move(behaviors.new_rigid_body_instances_by_path);
}

static void write_simulated_data_to_output(BulletState &state)
{
  for (RigidBodyInstances &rigid_body_instances : state.rigid_body_instances_by_path.values()) {
    bke::Instances *instances = rigid_body_instances.geometry_set.get_instances_for_write();
    if (!instances) {
      continue;
    }
    const int instances_num = instances->instances_num();
    const Span<int> instance_ids = instances->almost_unique_ids();
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

  update_state_from_behaviors(state, *behaviors_bundle);

  /* The Bullet state can't easily be reset to an older state. So better just don't do simulation
   * in this case. */
  const bool is_resimulating = update_counter < state.update_counter;
  update_counter++;
  if (!is_resimulating) {
    state.dynamics_world->stepSimulation(delta_time, substeps);
    state.update_counter = update_counter;
  }
  write_simulated_data_to_output(state);

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
