/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include <btBulletDynamicsCommon.h>

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

struct BulletState {
  bool is_initialized = false;
  std::unique_ptr<btDefaultCollisionConfiguration> collision_configuration;
  std::unique_ptr<btCollisionDispatcher> collision_dispatcher;
  std::unique_ptr<btDbvtBroadphase> broadphase;
  std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
  std::unique_ptr<btDiscreteDynamicsWorld> dynamics_world;
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

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr old_data_bundle = params.extract_input<BundlePtr>("Data");
  BundlePtr behaviors_bundle = params.extract_input<BundlePtr>("Behavior");

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

  BundlePtr new_data_bundle_ptr = Bundle::create();
  Bundle &new_data_bundle = const_cast<Bundle &>(*new_data_bundle_ptr);
  new_data_bundle.add("Bullet State", bullet_state_owner);

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
