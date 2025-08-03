/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "box2d/box2d.h"

#include "node_geometry_util.hh"

#include "NOD_geometry_nodes_physics_bundles.hh"

namespace blender::nodes::node_geo_box2d_physics_solver_cc {

using namespace physics_bundles;

static NestedBundleTypePtr make_world_type()
{
  Vector<std::shared_ptr<const FlatBundleType>> types;

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
  b.add_input<decl::Int>("Substeps").default_value(4).min(1);
}

static const bNodeSocket *node_internally_linked_input(const bNodeTree & /*tree*/,
                                                       const bNode &node,
                                                       const bNodeSocket &output_socket)
{
  /* Internal links should always map corresponding input and output sockets. */
  return node.input_by_identifier(output_socket.identifier);
}

class Box2DState {
 public:
  bool is_initialized = false;
  int update_counter = 0;

  b2WorldId world_id = b2_nullWorldId;

  b2BodyId ground_body_id = b2_nullBodyId;
  b2ShapeId ground_shape_id = b2_nullShapeId;

  b2BodyId box_body_id = b2_nullBodyId;
  b2ShapeId box_shape_id = b2_nullShapeId;

  ~Box2DState()
  {
    if (b2World_IsValid(this->world_id)) {
      b2DestroyWorld(this->world_id);
    }
  }
};

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
  const float delta_time = params.extract_input<float>("Delta Time");
  const int sub_steps = params.extract_input<int>("Substeps");

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
    state.world_id = b2CreateWorld(&world_def);

    b2BodyDef ground_body_def = b2DefaultBodyDef();
    state.ground_body_id = b2CreateBody(state.world_id, &ground_body_def);

    b2Polygon ground_box = b2MakeBox(10, 1);
    b2ShapeDef ground_shape_def = b2DefaultShapeDef();
    state.ground_shape_id = b2CreatePolygonShape(
        state.ground_body_id, &ground_shape_def, &ground_box);

    b2BodyDef box_body_def = b2DefaultBodyDef();
    box_body_def.type = b2_dynamicBody;
    box_body_def.position = (b2Vec2){0, 4};
    box_body_def.rotation = b2MakeRot(0.2f);
    state.box_body_id = b2CreateBody(state.world_id, &box_body_def);

    b2Polygon box_polygon = b2MakeBox(1, 1);
    b2ShapeDef box_shape_def = b2DefaultShapeDef();
    box_shape_def.density = 1.0f;
    box_shape_def.material.friction = 0.3f;
    state.box_shape_id = b2CreatePolygonShape(state.box_body_id, &box_shape_def, &box_polygon);

    state.is_initialized = true;
  }

  const bool is_resimulating = update_counter < state.update_counter;
  update_counter++;
  if (!is_resimulating) {
    b2World_Step(state.world_id, delta_time, sub_steps);

    b2Vec2 position = b2Body_GetPosition(state.box_body_id);
    b2Rot rotation = b2Body_GetRotation(state.box_body_id);
    printf("%4.2f %4.2f %4.2f\n", position.x, position.y, b2Rot_GetAngle(rotation));

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
