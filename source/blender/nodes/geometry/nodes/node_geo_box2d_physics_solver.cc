/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances.hh"
#include "BLI_math_matrix.hh"
#include "BLI_threads.h"
#include "box2d/box2d.h"

#include "xxhash.h"

#include "node_geometry_util.hh"

#include "NOD_geometry_nodes_physics_bundles.hh"

namespace blender::nodes::node_geo_box2d_physics_solver_cc {

using namespace physics_bundles;

static NestedBundleTypePtr make_world_type()
{
  Vector<std::shared_ptr<const FlatBundleType>> types;
  types.append(GravityBundle::get_bundle_type());
  types.append(ForceBundle::get_bundle_type());
  types.append(RigidBodyInstancesBundle::get_bundle_type());

  NestedBundleTypePtr world_type = std::make_shared<const NestedBundleType>(
      "Blender.Box2DSolverWorld", std::move(types));
  BundleTypeRegistry::register_type(world_type);
  return world_type;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  static NestedBundleTypePtr world_type = make_world_type();

  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bundle>("State").description(
      "Internal solver state. Has to be passed in from the previous iteration");
  b.add_output<decl::Bundle>("State").align_with_previous();
  b.add_input<decl::Bundle>("World")
      .bundle_type(world_type)
      .description("Simulation world description that is simulated");
  b.add_output<decl::Bundle>("World")
      .pass_through_input_index(1)
      .align_with_previous()
      .description("Simulated world");
  b.add_input<decl::Float>("Delta Time").min(0).hide_value();
  b.add_input<decl::Int>("Substeps").default_value(1).min(1);
  b.add_input<decl::Int>("Solver Steps").default_value(4).min(1);
}

static const bNodeSocket *node_internally_linked_input(const bNodeTree & /*tree*/,
                                                       const bNode &node,
                                                       const bNodeSocket &output_socket)
{
  /* Internal links should always map corresponding input and output sockets. */
  return node.input_by_identifier(output_socket.identifier);
}

struct RigidBodyForInstance {
  b2BodyId body_id = b2_nullBodyId;
  uint64_t polygon_hash = 0;
};

struct RigidBodiesForPath {
  Map<int, RigidBodyForInstance> bodies_by_id;
};

class Box2DState {
 public:
  bool is_initialized = false;
  int update_counter = 0;

  b2WorldId world_id = b2_nullWorldId;

  Map<std::string, RigidBodiesForPath> rigid_bodies_by_path;

  ~Box2DState()
  {
    if (b2World_IsValid(this->world_id)) {
      b2DestroyWorld(this->world_id);
    }
  }
};

struct WorldData {
  Vector<RigidBodyInstancesBundle> rigid_bodies;
};

static WorldData parse_world(const Bundle &world_bundle)
{
  WorldData world;
  nested_bundle_foreach(world_bundle, [&](HandleNestedBundleParams &params) {
    BundleParseErrors errors;
    if (params.type == RigidBodyInstancesBundle::name) {
      if (std::optional<RigidBodyInstancesBundle> rigid_body = RigidBodyInstancesBundle::parse(
              params.bundle, errors))
      {
        world.rigid_bodies.append(std::move(*rigid_body));
        world.rigid_bodies.last().self_path = Bundle::combine_path(params.path);
      }
    }
  });
  return world;
}

static GeometrySet apply_rigid_body_simulation(const RigidBodyInstancesBundle &bundle,
                                               const Box2DState &state)
{
  GeometrySet geometry = bundle.instances_geometry;
  bke::Instances *instances = geometry.get_instances_for_write();
  if (!instances) {
    return geometry;
  }

  const RigidBodiesForPath *rigid_bodies = state.rigid_bodies_by_path.lookup_ptr(bundle.self_path);
  if (!rigid_bodies) {
    return geometry;
  }

  const int instances_num = instances->instances_num();
  const Span<int> instance_ids = instances->almost_unique_ids();
  MutableSpan<float4x4> transforms = instances->transforms_for_write();

  for (const int instance_i : IndexRange(instances_num)) {
    const int instance_id = instance_ids[instance_i];
    const RigidBodyForInstance *rigid_body = rigid_bodies->bodies_by_id.lookup_ptr(instance_id);
    if (!rigid_body) {
      continue;
    }
    const b2BodyId body_id = rigid_body->body_id;
    if (b2Body_GetType(body_id) != b2_dynamicBody) {
      continue;
    }
    const b2Transform b2_transform = b2Body_GetTransform(body_id);

    float4x4 &transform = transforms[instance_i];
    const float3 old_scale = math::to_scale(transform);
    const float3 old_position = transform.location();

    transform = float4x4::identity();
    transform[0][0] = b2_transform.q.c * old_scale.x;
    transform[1][0] = -b2_transform.q.s * old_scale.y;
    transform[0][1] = b2_transform.q.s * old_scale.x;
    transform[1][1] = b2_transform.q.c * old_scale.y;
    transform[2][2] = old_scale.z;
    transform[3][0] = b2_transform.p.x;
    transform[3][1] = b2_transform.p.y;
    transform[3][2] = old_position.z;
  }
  return geometry;
}

/* Keep in sync with jolt motion types. */
static std::optional<b2BodyType> parse_body_type(const int type)
{
  switch (type) {
    case 0:
      return b2_dynamicBody;
    case 1:
      return b2_staticBody;
    case 2:
      return b2_kinematicBody;
    default:
      return std::nullopt;
  }
}

static float get_angular_difference(const float from, const float to)
{
  float d = to - from;
  if (d > 2 * M_PI || d < -2 * M_PI) {
    d = fmodf(d, 2 * M_PI);
  }
  if (d < -M_PI) {
    d += 2 * M_PI;
  }
  if (d > M_PI) {
    d -= 2 * M_PI;
  }
  return d;
}

static b2BodyType convert_to_body_type(const RigidBodyMotionType motion_type)
{
  switch (motion_type) {
    case RigidBodyMotionType::Dynamic:
      return b2_dynamicBody;
    case RigidBodyMotionType::Static:
      return b2_staticBody;
    case RigidBodyMotionType::Animated:
      return b2_kinematicBody;
  }
  BLI_assert_unreachable();
  return {};
}

static void handle_rigid_body_instances_bundle(
    Box2DState &state,
    const RigidBodyInstancesBundle &bundle,
    const bke::Instances &current_instances,
    const float delta_time,
    Map<std::string, RigidBodiesForPath> &r_rigid_bodies_by_path)
{
  const bke::Instances *original_instances = bundle.instances_geometry.get_instances();
  if (!original_instances) {
    return;
  }

  const int instances_num = current_instances.instances_num();
  const int references_num = current_instances.references_num();
  const Span<int> instance_ids = current_instances.almost_unique_ids();
  const Span<float4x4> transforms = current_instances.transforms();
  const Span<bke::InstanceReference> references = current_instances.references();
  const Span<int> handles = current_instances.reference_handles();
  const Span<float4x4> original_transforms = original_instances->transforms();

  bke::InstancesFieldContext field_context(current_instances);
  fn::FieldEvaluator field_evaluator{field_context, instances_num};
  field_evaluator.add(bundle.collision_shape_type);
  field_evaluator.add(bundle.motion_type);
  field_evaluator.add(bundle.friction);
  field_evaluator.add(bundle.bounciness);
  field_evaluator.add(bundle.density);
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

  RigidBodiesForPath *old_rigid_bodies = state.rigid_bodies_by_path.lookup_ptr(bundle.self_path);

  RigidBodiesForPath rigid_bodies;
  for (const int instance_i : IndexRange(instances_num)) {
    const int reference_i = handles[instance_i];
    const int instance_id = instance_ids[instance_i];
    if (!reference_geometry_sets.index_range().contains(reference_i)) {
      continue;
    }
    const std::optional<RigidBodyMotionType> motion_type =
        RigidBodyInstancesBundle::parse_motion_type(motion_types[instance_i]);
    if (!motion_type) {
      continue;
    }
    const b2BodyType body_type = convert_to_body_type(*motion_type);
    const GeometrySet &reference_geometry = reference_geometry_sets[reference_i];
    const std::optional<Bounds<float3>> bounds =
        reference_geometry.compute_boundbox_without_instances(true);
    if (!bounds) {
      continue;
    }
    const float4x4 &instance_transform = transforms[instance_i];
    const float4x4 &original_transform = original_transforms[instance_i];
    float3 instance_position;
    math::EulerXYZ instance_rotation;
    float3 instance_scale;
    math::to_loc_rot_scale_safe<true>(
        instance_transform, instance_position, instance_rotation, instance_scale);

    const float3 original_scale = math::to_scale(original_transform);

    float density = densities[instance_i];
    if (density <= 0.0f) {
      density = 1.0f;
    }
    const float friction = std::max(frictions[instance_i], 0.0f);
    const float bounciness = std::max(bouncinesses[instance_i], 0.0f);

    const float half_width = math::abs(
        math::max(math::abs(bounds->min.x), math::abs(bounds->max.x)) * original_scale.x);
    const float half_height = math::abs(
        math::max(math::abs(bounds->min.y), math::abs(bounds->max.y)) * original_scale.y);
    if (half_width <= 0.0f || half_height <= 0.0f) {
      continue;
    }

    const b2Polygon polygon = b2MakeBox(half_width, half_height);
    const uint64_t polygon_hash = XXH3_64bits(&polygon, sizeof(b2Polygon));

    std::optional<RigidBodyForInstance> rigid_body;
    if (old_rigid_bodies) {
      if (std::optional<RigidBodyForInstance> old_rigid_body =
              old_rigid_bodies->bodies_by_id.pop_try(instance_id))
      {
        rigid_body = old_rigid_body;
        b2BodyId body_id = rigid_body->body_id;
        const b2BodyType old_body_type = b2Body_GetType(body_id);
        if (old_body_type != body_type) {
          b2Body_SetType(body_id, body_type);
          if (old_body_type == b2_staticBody) {
            b2Body_SetTransform(body_id,
                                b2Vec2{instance_position.x, instance_position.y},
                                b2MakeRot(instance_rotation.z().radian()));
          }
        }
      }
    }
    if (!rigid_body) {
      b2BodyDef body_def = b2DefaultBodyDef();
      body_def.type = body_type;
      body_def.position.x = instance_position.x;
      body_def.position.y = instance_position.y;
      body_def.rotation = b2MakeRot(instance_rotation.z().radian());
      b2BodyId body_id = b2CreateBody(state.world_id, &body_def);

      b2ShapeDef shape_def = b2DefaultShapeDef();
      shape_def.density = density;
      shape_def.material.friction = friction;
      shape_def.material.restitution = bounciness;
      b2CreatePolygonShape(body_id, &shape_def, &polygon);

      rigid_body = {body_id, polygon_hash};
    }

    b2BodyId body_id = rigid_body->body_id;
    b2ShapeId shape_id;
    b2Body_GetShapes(body_id, &shape_id, 1);
    if (b2Shape_GetFriction(shape_id) != friction) {
      b2Shape_SetFriction(shape_id, friction);
      b2Body_EnableSleep(body_id, false);
    }
    if (b2Shape_GetRestitution(shape_id) != bounciness) {
      b2Shape_SetRestitution(shape_id, bounciness);
      b2Body_EnableSleep(body_id, false);
    }
    if (b2Shape_GetType(shape_id) == b2_polygonShape) {
      if (polygon_hash != rigid_body->polygon_hash) {
        b2Shape_SetPolygon(shape_id, &polygon);
        rigid_body->polygon_hash = polygon_hash;
      }
    }
    if (body_type == b2_kinematicBody && delta_time > 0.0f) {
      const b2Transform last_transform = b2Body_GetTransform(body_id);
      const float2 linear_delta = instance_position.xy() -
                                  float2(last_transform.p.x, last_transform.p.y);
      const float angular_delta = get_angular_difference(b2Rot_GetAngle(last_transform.q),
                                                         instance_rotation.z().radian());
      const float2 velocity = linear_delta / delta_time;
      const float angular_velocity = angular_delta / delta_time;
      b2Body_SetLinearVelocity(body_id, b2Vec2{velocity.x, velocity.y});
      b2Body_SetAngularVelocity(body_id, angular_velocity);
    }
    if (body_type == b2_staticBody) {
      b2Body_SetTransform(body_id,
                          b2Vec2{instance_position.x, instance_position.y},
                          b2MakeRot(instance_rotation.z().radian()));
    }

    rigid_bodies.bodies_by_id.add(instance_id, std::move(*rigid_body));
  }
  r_rigid_bodies_by_path.add(bundle.self_path, std::move(rigid_bodies));
}

static void update_box2d_state_from_world(Box2DState &state,
                                          const WorldData &world,
                                          const float delta_time)
{
  Array<GeometrySet> applied_rigid_bodies(world.rigid_bodies.size());
  for (const int i : world.rigid_bodies.index_range()) {
    const RigidBodyInstancesBundle &rigid_body_bundle = world.rigid_bodies[i];
    applied_rigid_bodies[i] = apply_rigid_body_simulation(rigid_body_bundle, state);
  }

  Map<std::string, RigidBodiesForPath> new_rigid_bodies_by_path;
  for (const int i : world.rigid_bodies.index_range()) {
    const GeometrySet &applied_rigid_body = applied_rigid_bodies[i];
    const bke::Instances *instances = applied_rigid_body.get_instances();
    if (!instances) {
      continue;
    }
    const RigidBodyInstancesBundle &rigid_body_bundle = world.rigid_bodies[i];
    handle_rigid_body_instances_bundle(
        state, rigid_body_bundle, *instances, delta_time, new_rigid_bodies_by_path);
  }

  /* Remove old data.*/
  for (RigidBodiesForPath &rigid_bodies : state.rigid_bodies_by_path.values()) {
    for (RigidBodyForInstance &rigid_body : rigid_bodies.bodies_by_id.values()) {
      b2DestroyBody(rigid_body.body_id);
    }
  }

  state.rigid_bodies_by_path = std::move(new_rigid_bodies_by_path);
}

class Box2DStateOwner : public BundleItemInternalValueMixin {
 public:
  mutable Mutex mutex;
  mutable Box2DState state;

  void delete_self() override
  {
    MEM_delete(this);
  }

  StringRefNull type_name() const override
  {
    return TIP_("Box2D Physics State");
  }
};
using Box2DStateOwnerPtr = ImplicitSharingPtr<Box2DStateOwner>;

static void node_geo_exec_locked(GeoNodeExecParams params)
{
  BundlePtr old_state_bundle_ptr = params.extract_input<BundlePtr>("State");
  BundlePtr world_bundle_ptr = params.extract_input<BundlePtr>("World");
  const float delta_time = std::max(0.0f, params.extract_input<float>("Delta Time"));
  const int sub_steps = std::max(1, params.extract_input<int>("Substeps"));
  const int solver_steps = std::max(1, params.extract_input<int>("Solver Steps"));

  if (!world_bundle_ptr) {
    params.set_default_remaining_outputs();
    return;
  }

  int update_counter = 0;
  if (old_state_bundle_ptr) {
    update_counter = old_state_bundle_ptr->lookup<int>("_counter").value_or(0);
  }

  Box2DStateOwnerPtr box2d_state_owner;
  if (old_state_bundle_ptr) {
    box2d_state_owner = old_state_bundle_ptr->lookup<Box2DStateOwnerPtr>("_state").value_or(
        nullptr);
  }
  if (!box2d_state_owner) {
    box2d_state_owner = Box2DStateOwnerPtr{MEM_new<Box2DStateOwner>(__func__)};
  }

  if (!box2d_state_owner->mutex.try_lock()) {
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Box2D physics state cannot be used by multiple nodes"));
    params.set_default_remaining_outputs();
    return;
  }
  BLI_SCOPED_DEFER([&]() { box2d_state_owner->mutex.unlock(); });

  Box2DState &state = box2d_state_owner->state;
  if (!state.is_initialized) {
    b2WorldDef world_def = b2DefaultWorldDef();
    world_def.workerCount = BLI_system_thread_count();
    world_def.enqueueTask = [](b2TaskCallback *task,
                               const int item_count,
                               const int min_range,
                               void *task_context,
                               void * /*user_context*/) -> void * {
      threading::parallel_for(IndexRange(item_count), min_range, [&](const IndexRange range) {
        static std::atomic<int> worker_counter = 0;
        static thread_local int worker_index = worker_counter.fetch_add(1);
        task(range.start(), range.one_after_last(), worker_index, task_context);
      });
      return nullptr;
    };
    world_def.finishTask = [](void * /*user_task*/, void * /*user_context*/) -> void {
      /* All tasks are done eagerly. */
    };
    state.world_id = b2CreateWorld(&world_def);
    state.is_initialized = true;
  }

  WorldData world = parse_world(*world_bundle_ptr);

  const bool is_resimulating = update_counter < state.update_counter;
  update_counter++;
  if (!is_resimulating) {
    update_box2d_state_from_world(state, world, delta_time);
    for ([[maybe_unused]] const int i : IndexRange(sub_steps)) {
      b2World_Step(state.world_id, delta_time / sub_steps, solver_steps);
    }
    state.update_counter = update_counter;
  }

  BundlePtr new_state_bundle_ptr = Bundle::create();
  Bundle &new_state_bundle = const_cast<Bundle &>(*new_state_bundle_ptr);
  new_state_bundle.add("_state", box2d_state_owner);
  new_state_bundle.add("_counter", update_counter);

  if (!world_bundle_ptr->is_mutable()) {
    world_bundle_ptr = world_bundle_ptr->copy();
  }
  else {
    world_bundle_ptr->tag_ensured_mutable();
  }
  Bundle &world_bundle = const_cast<Bundle &>(*world_bundle_ptr);
  for (RigidBodyInstancesBundle &rigid_bodies_bundle : world.rigid_bodies) {
    GeometrySet applied_rigid_bodies = apply_rigid_body_simulation(rigid_bodies_bundle, state);
    world_bundle.add_path_override(rigid_bodies_bundle.self_path + "/instances",
                                   std::move(applied_rigid_bodies));
  }

  params.set_output("State", std::move(new_state_bundle_ptr));
  params.set_output("World", std::move(world_bundle_ptr));
}

static void node_geo_exec(GeoNodeExecParams params)
{
  /* The entire Box2D API is single-threaded, so use a global lock for this node. */
  static Mutex box2d_mutex;
  std::scoped_lock lock(box2d_mutex);
  threading::isolate_task([&]() { node_geo_exec_locked(params); });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeBox2DPhysicsSolver");
  ntype.ui_name = "Box2D Physics Solver";
  ntype.ui_description = "Simulate physics using the Box2D physics engine";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.internally_linked_input = node_internally_linked_input;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_box2d_physics_solver_cc
