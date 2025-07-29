/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances.hh"
#include "BLI_math_matrix.hh"
#include "NOD_geometry_nodes_behaviors_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"

#include "node_geometry_util.hh"

#include "Jolt/Jolt.h"
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

namespace blender::nodes::node_geo_jolt_physics_solver_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bundle>("Data");
  b.add_output<decl::Bundle>("Data").align_with_previous();
  b.add_input<decl::Bundle>("Behavior");
  b.add_input<decl::Float>("Delta Time").min(0).hide_value();
  b.add_input<decl::Int>("Substeps").default_value(1).min(1);
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
  JPH::Body *body;
};

struct JoltRigidBodies {
  Map<int, JoltRigidBody> bodies_by_id;
};

struct JoltState {
  bool is_initialized = false;

  BroadPhaseLayerInterfaceImpl broad_phase_layer_interface;
  ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter;
  ObjectLayerPairFilterImpl object_layer_pair_filter;
  BodyActivationListenerImpl body_activation_listener;
  ContactListenerImpl contact_listener;
  JPH::PhysicsSystem system;

  Map<std::string, JoltRigidBodies> rigid_bodies_by_path;
};

struct RigidBodiesBehavior {
  std::string self_path;
  GeometrySet geometry;
};

struct JoltBehaviors {
  Vector<RigidBodiesBehavior> rigid_bodies;
};

struct ParseBehaviorParams {
  const Span<StringRef> path_elems;
  const Bundle &bundle;
  JoltBehaviors &r_behaviors;

  std::string self_path() const
  {
    return Bundle::combine_path(this->path_elems);
  }
};

using ParseBehaviorFn = std::function<void(ParseBehaviorParams &params)>;

static void parse_behavior__rigid_bodies(ParseBehaviorParams &params)
{
  std::optional<GeometrySet> geometry = params.bundle.lookup<GeometrySet>("Instances");
  if (!geometry) {
    return;
  }
  geometry->keep_only({bke::GeometryComponent::Type::Instance});

  RigidBodiesBehavior rigid_bodies_behaviors;
  rigid_bodies_behaviors.self_path = params.self_path();
  rigid_bodies_behaviors.geometry = *geometry;
  params.r_behaviors.rigid_bodies.append(std::move(rigid_bodies_behaviors));
}

static Map<std::string, ParseBehaviorFn> build_behavior_parsers()
{
  Map<std::string, ParseBehaviorFn> behavior_parsers;
  behavior_parsers.add_new("Rigid Bodies", parse_behavior__rigid_bodies);
  return behavior_parsers;
}

static JoltBehaviors parse_behaviors(const Bundle &behaviors_bundle)
{
  JoltBehaviors behaviors;
  static Map<std::string, ParseBehaviorFn> behavior_parsers = build_behavior_parsers();
  behaviors::foreach_behavior_in_bundle(
      behaviors_bundle,
      [&](const StringRef type, const Bundle &behaviors_bundle, const Span<StringRef> path) {
        if (const ParseBehaviorFn *parse = behavior_parsers.lookup_ptr_as(type)) {
          ParseBehaviorParams params{path, behaviors_bundle, behaviors};
          (*parse)(params);
        }
      });
  return behaviors;
}

static void handle_rigid_bodies_behavior(JoltState &state,
                                         const RigidBodiesBehavior &behavior,
                                         Map<std::string, JoltRigidBodies> &r_rigid_bodies_by_path)
{
  JPH::BodyInterface &body_interface = state.system.GetBodyInterfaceNoLock();

  const bke::Instances *instances = behavior.geometry.get_instances();
  if (!instances) {
    return;
  }
  const int instances_num = instances->instances_num();
  const int references_num = instances->references_num();
  const Span<int> instance_ids = instances->almost_unique_ids();
  const Span<float4x4> transforms = instances->transforms();
  const Span<bke::InstanceReference> references = instances->references();
  const Span<int> handles = instances->reference_handles();

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
    const GeometrySet &reference_geometry = reference_geometry_sets[reference_i];
    const std::optional<Bounds<float3>> bounds =
        reference_geometry.compute_boundbox_without_instances(true);
    if (!bounds) {
      continue;
    }
    const float4x4 &raw_transform = transforms[instance_i];
    const float3 &position = raw_transform.location();

    const float3 half_extent = bounds->size() / 2.0f;
    const JPH::BoxShapeSettings box_shape_settings{
        JPH::Vec3(half_extent.x, half_extent.y, half_extent.z)};
    JPH::ShapeSettings::ShapeResult box_shape = box_shape_settings.Create();
    if (!box_shape.IsValid()) {
      continue;
    }

    std::optional<JoltRigidBody> rigid_body;
    if (old_rigid_bodies) {
      if (std::optional<JoltRigidBody> old_rigid_body = old_rigid_bodies->bodies_by_id.pop(
              instance_i))
      {
        rigid_body = old_rigid_body;
      }
    }
    if (!rigid_body) {
      JPH::BodyCreationSettings jolt_body_settings{box_shape.Get(),
                                                   JPH::Vec3(position.x, position.y, position.z),
                                                   JPH::Quat::sIdentity(),
                                                   JPH::EMotionType::Dynamic,
                                                   ObjectLayers::moving};

      JPH::Body *jolt_body = body_interface.CreateBody(jolt_body_settings);
      body_interface.AddBody(jolt_body->GetID(), JPH::EActivation::Activate);
      rigid_body = JoltRigidBody{jolt_body};
    }
    rigid_bodies.bodies_by_id.add(instance_id, std::move(*rigid_body));
  }

  r_rigid_bodies_by_path.add(behavior.self_path, std::move(rigid_bodies));
}

static void update_jolt_state_from_behaviors(JoltState &state, const JoltBehaviors &behaviors)
{
  Map<std::string, JoltRigidBodies> new_rigid_bodies_by_path;

  JPH::BodyInterface &body_interface = state.system.GetBodyInterfaceNoLock();

  for (const RigidBodiesBehavior &rigid_bodies_behaviors : behaviors.rigid_bodies) {
    handle_rigid_bodies_behavior(state, rigid_bodies_behaviors, new_rigid_bodies_by_path);
  }

  /* Remove old rigid bodies. */
  Vector<JPH::BodyID> bodies_to_remove;
  for (JoltRigidBodies &rigid_bodies : state.rigid_bodies_by_path.values()) {
    for (JoltRigidBody &body : rigid_bodies.bodies_by_id.values()) {
      bodies_to_remove.append(body.body->GetID());
    }
  }
  body_interface.RemoveBodies(bodies_to_remove.data(), bodies_to_remove.size());
  body_interface.DestroyBodies(bodies_to_remove.data(), bodies_to_remove.size());

  state.rigid_bodies_by_path = std::move(new_rigid_bodies_by_path);
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

static GeometrySet merge_simulation_data_into_behavior_geometry(
    const RigidBodiesBehavior &behavior, const JoltState &state)
{
  GeometrySet geometry = behavior.geometry;
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

    const float3 position = float3(
        jolt_position.GetX(), jolt_position.GetY(), jolt_position.GetZ());
    const math::Quaternion rotation = math::Quaternion(
        jolt_rotation.GetW(), jolt_rotation.GetX(), jolt_rotation.GetY(), jolt_rotation.GetZ());

    /* Scale is not simulated to Jolt, so keep the scale of the original geometry. */
    const float3 scale = math::to_scale(transform);

    transform = math::from_loc_rot_scale<float4x4>(position, rotation, scale);
  }

  return geometry;
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
    state.is_initialized = true;
  }

  JoltBehaviors behaviors = parse_behaviors(*behavior_bundle);
  update_jolt_state_from_behaviors(state, behaviors);

  {
    JPH::TempAllocatorImpl temp_allocator(10 * 1024 * 1024);
    /* TODO: Integrate with TBB. */
    JPH::JobSystemThreadPool job_system(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);
    const int collision_steps = sub_steps;
    state.system.Update(delta_time, collision_steps, &temp_allocator, &job_system);
  }

  BundlePtr new_data_bundle_ptr = Bundle::create();
  Bundle &new_data_bundle = const_cast<Bundle &>(*new_data_bundle_ptr);

  for (const RigidBodiesBehavior &rigid_bodies_behavior : behaviors.rigid_bodies) {
    const GeometrySet geometry = merge_simulation_data_into_behavior_geometry(
        rigid_bodies_behavior, state);
    new_data_bundle.add_path_override(rigid_bodies_behavior.self_path + "/Instances", geometry);
  }

  new_data_bundle.add("_state", jolt_state_owner);
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
