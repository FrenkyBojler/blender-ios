/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_bundle.hh"
#include "node_geometry_util.hh"

#include "Jolt/Jolt.h"
#include <Jolt/Core/Factory.h>
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

struct JoltState {};

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

  // JPH::RegisterDefaultAllocator();
  // JPH::Factory::sInstance = new JPH::Factory();
  // JPH::RegisterTypes();

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
