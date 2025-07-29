/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_behaviors_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"

#include "node_geometry_util.hh"

#include "Jolt/Jolt.h"
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
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

struct JoltState {
  bool is_initialized = false;

  BroadPhaseLayerInterfaceImpl broad_phase_layer_interface;
  ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter;
  ObjectLayerPairFilterImpl object_layer_pair_filter;
  BodyActivationListenerImpl body_activation_listener;
  ContactListenerImpl contact_listener;
  JPH::PhysicsSystem system;
};

struct RigidBodiesBehaviors {
  std::string self_path;
  GeometrySet geometry;
};

struct JoltBehaviors {
  Vector<RigidBodiesBehaviors> rigid_bodies;
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
  RigidBodiesBehaviors rigid_bodies_behaviors;
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
    JPH::Allocate = [](size_t size) { return MEM_mallocN(size, func); };
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
    jolt_state_owner = old_data_bundle->lookup<JoltStateOwnerPtr>("Jolt State").value_or(nullptr);
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
