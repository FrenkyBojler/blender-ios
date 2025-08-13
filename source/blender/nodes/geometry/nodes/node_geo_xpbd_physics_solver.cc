/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"

#include "DNA_mesh_types.h"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"

#include "intern/attribute_storage_access.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_xpbd_physics_solver_cc {

using namespace physics_bundles;

static NestedBundleTypePtr make_world_type()
{
  Vector<std::shared_ptr<const FlatBundleType>> types;
  types.append(GravityBundle::get_bundle_type());
  types.append(ForceBundle::get_bundle_type());
  types.append(XPBDGeometryBundle::get_bundle_type());
  types.append(EdgeLengthXPBDConstraintBundle::get_bundle_type());
  types.append(PinnedPositionXPBDConstraintBundle::get_bundle_type());

  NestedBundleTypePtr world_type = std::make_shared<const NestedBundleType>(
      "Blender.XpbdSolverWorld", std::move(types));
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
  b.add_input<decl::Float>("Delta Time").min(0).default_value(1 / 25.0f);
  b.add_input<decl::Int>("Substeps").default_value(1).min(1);
}

struct SimPoints {
  int points_num;
  Array<float3> positions;
  Array<float3> velocities;
};

struct PathSimData {
  Map<bke::GeometryComponent::Type, SimPoints> points_by_type;
};

class XPBDState {
 public:
  /**
   * Counts how often this state has been updated. This is mainly used to avoid re-simulating the
   * same frame multiple times when playback is paused but the simulation parameters are changed.
   */
  int update_counter = 0;

  Map<std::string, PathSimData> data_by_path;
};

class XPBDStateOwner : public BundleItemInternalValueMixin {
 public:
  mutable Mutex mutex;
  mutable XPBDState state;

  void delete_self() override
  {
    MEM_delete(this);
  }

  StringRefNull type_name() const override
  {
    return TIP_("XPBD Physics State");
  }
};
using XPBDStateOwnerPtr = ImplicitSharingPtr<XPBDStateOwner>;

struct WorldData {
  Vector<ForceBundle> forces;
  Vector<GravityBundle> gravities;
  Vector<XPBDGeometryBundle> geometries;
  Vector<EdgeLengthXPBDConstraintBundle> edge_length_constraints;
  Vector<PinnedPositionXPBDConstraintBundle> pinned_position_constraints;
};

static WorldData parse_world(const Bundle &world_bundle)
{
  WorldData world;
  nested_bundle_foreach(world_bundle, [&](HandleNestedBundleParams &params) {
    BundleParseErrors errors;
    if (params.type == ForceBundle::name) {
      if (std::optional<ForceBundle> force = ForceBundle::parse(params.bundle, errors)) {
        world.forces.append(std::move(*force));
        world.forces.last().self_path = Bundle::combine_path(params.path);
      }
    }
    else if (params.type == GravityBundle::name) {
      if (std::optional<GravityBundle> gravity = GravityBundle::parse(params.bundle, errors)) {
        world.gravities.append(std::move(*gravity));
        world.gravities.last().self_path = Bundle::combine_path(params.path);
      }
    }
    else if (params.type == XPBDGeometryBundle::name) {
      if (std::optional<XPBDGeometryBundle> geometry = XPBDGeometryBundle::parse(params.bundle,
                                                                                 errors))
      {
        world.geometries.append(std::move(*geometry));
        world.geometries.last().self_path = Bundle::combine_path(params.path);
      }
    }
    else if (params.type == EdgeLengthXPBDConstraintBundle::name) {
      if (std::optional<EdgeLengthXPBDConstraintBundle> constraint =
              EdgeLengthXPBDConstraintBundle::parse(params.bundle, errors))
      {
        world.edge_length_constraints.append(std::move(*constraint));
        world.edge_length_constraints.last().self_path = Bundle::combine_path(params.path);
      }
    }
    else if (params.type == PinnedPositionXPBDConstraintBundle::name) {
      if (std::optional<PinnedPositionXPBDConstraintBundle> constraint =
              PinnedPositionXPBDConstraintBundle::parse(params.bundle, errors))
      {
        world.pinned_position_constraints.append(std::move(*constraint));
        world.pinned_position_constraints.last().self_path = Bundle::combine_path(params.path);
      }
    }
  });
  return world;
}

static void apply_simulation_to_mesh(GeometrySet &geometry, const PathSimData &path_sim_data)
{
  if (!geometry.has_mesh()) {
    return;
  }
  const SimPoints *sim_points = path_sim_data.points_by_type.lookup_ptr(
      bke::GeometryComponent::Type::Mesh);
  if (!sim_points) {
    return;
  }
  if (geometry.get_mesh()->verts_num != sim_points->points_num) {
    return;
  }
  Mesh *mesh = geometry.get_mesh_for_write();
  MutableSpan<float3> mesh_positions = mesh->vert_positions_for_write();
  mesh_positions.copy_from(sim_points->positions);
  mesh->tag_positions_changed();
}

static GeometrySet apply_simulation(const XPBDGeometryBundle &bundle, const XPBDState &state)
{
  GeometrySet geometry = bundle.geometry;

  const PathSimData *path_sim_data = state.data_by_path.lookup_ptr(bundle.self_path);
  if (!path_sim_data) {
    return geometry;
  }

  apply_simulation_to_mesh(geometry, *path_sim_data);

  return geometry;
}

static void update_xpbd_state_for_geometry(XPBDState &state,
                                           const XPBDGeometryBundle &bundle,
                                           const GeometrySet &current_geometry,
                                           Map<std::string, PathSimData> &r_data_by_path)
{
  PathSimData new_path_sim_data;

  PathSimData *old_path_sim_data = state.data_by_path.lookup_ptr(bundle.self_path);

  if (current_geometry.has_mesh()) {
    const Mesh *current_mesh = current_geometry.get_mesh();
    std::optional<SimPoints> new_sim_points;
    if (old_path_sim_data) {
      if (std::optional<SimPoints> old_sim_points = old_path_sim_data->points_by_type.pop_try(
              bke::GeometryComponent::Type::Mesh))
      {
        if (current_mesh->verts_num == old_sim_points->points_num) {
          new_sim_points = std::move(*old_sim_points);
        }
      }
    }
    if (!new_sim_points) {
      SimPoints sim_points;
      sim_points.points_num = current_mesh->verts_num;
      sim_points.positions = current_mesh->vert_positions();
      sim_points.velocities.reinitialize(current_mesh->verts_num);
      sim_points.velocities.fill(float3(0.0f));
      new_sim_points = std::move(sim_points);
    }
    new_path_sim_data.points_by_type.add_new(bke::GeometryComponent::Type::Mesh,
                                             std::move(*new_sim_points));
  }

  r_data_by_path.add(bundle.self_path, std::move(new_path_sim_data));
}

static Vector<const ForceBundle *> get_forces_for_path(const WorldData &world,
                                                       const StringRef path)
{
  Vector<const ForceBundle *> used_forces;
  for (const ForceBundle &force_bundle : world.forces) {
    if (nested_bundle_path_is_selected(force_bundle.self_path, force_bundle.filter, path)) {
      used_forces.append(&force_bundle);
    }
  }
  return used_forces;
}

static void integrate_forces_and_accelerations(XPBDState &state,
                                               const WorldData &world,
                                               Span<GeometrySet> applied_geometries,
                                               const float delta_time)
{
  float3 gravity;
  for (const GravityBundle &gravity_bundle : world.gravities) {
    gravity = gravity_bundle.gravity;
  }

  for (const int i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[i];
    const GeometrySet &applied_geometry = applied_geometries[i];
    PathSimData *path_sim_data = state.data_by_path.lookup_ptr(geometry_bundle.self_path);
    if (!path_sim_data) {
      continue;
    }
    Vector<const ForceBundle *> used_forces = get_forces_for_path(world,
                                                                  geometry_bundle.self_path);
    for (const bke::GeometryComponent::Type type : {bke::GeometryComponent::Type::Mesh}) {
      const bke::GeometryComponent *component = applied_geometry.get_component(type);
      if (!component) {
        continue;
      }
      SimPoints *sim_points = path_sim_data->points_by_type.lookup_ptr(type);
      if (!sim_points) {
        continue;
      }
      Array<float3> force_sums(sim_points->points_num, float3(0.0f));
      bke::GeometryFieldContext field_context(*component, bke::AttrDomain::Point);
      for (const ForceBundle *force_bundle : used_forces) {
        fn::FieldEvaluator field_evaluator{field_context, sim_points->points_num};
        field_evaluator.set_selection(force_bundle->selection);
        field_evaluator.add(force_bundle->force);
        field_evaluator.evaluate();
        const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
        const VArray<float3> force = field_evaluator.get_evaluated<float3>(0);
        mask.foreach_index(GrainSize(1024), [&](const int i) { force_sums[i] += force[i]; });
      }
      threading::parallel_for(
          IndexRange(sim_points->points_num), 1024, [&](const IndexRange range) {
            for (const int i : range) {
              const float mass = 1.0f;
              const float3 &force = force_sums[i];
              const float3 acceleration = force / mass + gravity;
              const float3 velocity_delta = acceleration * delta_time;
              float3 &velocity = sim_points->velocities[i];
              velocity += velocity_delta;
              sim_points->positions[i] += velocity * delta_time;
            }
          });
    }
  }
}

static void update_and_step_xpbd_state(XPBDState &state,
                                       const WorldData &world,
                                       const float delta_time,
                                       const int substeps)
{
  Array<GeometrySet> applied_geometries(world.geometries.size());
  for (const int i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[i];
    applied_geometries[i] = apply_simulation(geometry_bundle, state);
  }

  Map<std::string, PathSimData> new_data_by_path;
  for (const int i : world.geometries.index_range()) {
    const GeometrySet &applied_geometry = applied_geometries[i];
    update_xpbd_state_for_geometry(state, world.geometries[i], applied_geometry, new_data_by_path);
  }
  state.data_by_path = std::move(new_data_by_path);

  integrate_forces_and_accelerations(state, world, applied_geometries, delta_time);
}

static void initialize_state(XPBDState & /*state*/)
{
  /* Nothing to do yet.*/
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr old_state_bundle_ptr = params.extract_input<BundlePtr>("State");
  BundlePtr world_bundle_ptr = params.extract_input<BundlePtr>("World");
  const float delta_time = std::max(0.0f, params.extract_input<float>("Delta Time"));
  const int substeps = std::max(1, params.extract_input<int>("Substeps"));

  if (!world_bundle_ptr) {
    params.set_default_remaining_outputs();
    return;
  }

  int update_counter = 0;
  if (old_state_bundle_ptr) {
    update_counter = old_state_bundle_ptr->lookup<int>("counter").value_or(0);
  }

  XPBDStateOwnerPtr xpbd_state_owner;
  if (old_state_bundle_ptr) {
    xpbd_state_owner = old_state_bundle_ptr->lookup<XPBDStateOwnerPtr>("state").value_or(nullptr);
  }
  if (!xpbd_state_owner) {
    xpbd_state_owner = XPBDStateOwnerPtr{MEM_new<XPBDStateOwner>(__func__)};
    XPBDState &state = xpbd_state_owner->state;
    initialize_state(state);
  }

  if (!xpbd_state_owner->mutex.try_lock()) {
    params.error_message_add(NodeWarningType::Error,
                             TIP_("XPBD physics state cannot be used by multiple nodes"));
    params.set_default_remaining_outputs();
    return;
  }
  BLI_SCOPED_DEFER([&]() { xpbd_state_owner->mutex.unlock(); });

  WorldData world = parse_world(*world_bundle_ptr);
  XPBDState &state = xpbd_state_owner->state;

  const bool is_resimulating = update_counter < state.update_counter;
  update_counter++;
  if (!is_resimulating) {
    update_and_step_xpbd_state(state, world, delta_time, substeps);
    state.update_counter = update_counter;
  }

  BundlePtr new_state_bundle_ptr = Bundle::create();
  BLI_assert(new_state_bundle_ptr->is_mutable());
  Bundle &new_state_bundle = const_cast<Bundle &>(*new_state_bundle_ptr);
  new_state_bundle.add("state", xpbd_state_owner);
  new_state_bundle.add("counter", update_counter);

  if (!world_bundle_ptr->is_mutable()) {
    world_bundle_ptr = world_bundle_ptr->copy();
  }
  else {
    world_bundle_ptr->tag_ensured_mutable();
  }
  Bundle &world_bundle = const_cast<Bundle &>(*world_bundle_ptr);
  for (XPBDGeometryBundle &bundle : world.geometries) {
    GeometrySet applied_geometry = apply_simulation(bundle, state);
    world_bundle.add_path_override(bundle.self_path + "/geometry", std::move(applied_geometry));
  }

  params.set_output("State", std::move(new_state_bundle_ptr));
  params.set_output("World", std::move(world_bundle_ptr));
}

static const bNodeSocket *node_internally_linked_input(const bNodeTree & /*tree*/,
                                                       const bNode &node,
                                                       const bNodeSocket &output_socket)
{
  /* Internal links should always map corresponding input and output sockets. */
  return node.input_by_identifier(output_socket.identifier);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeXPBDPhysicsSolver");
  ntype.ui_name = "XPBD Physics Solver";
  ntype.ui_description = "Simulate physics using the XPBD framework";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.internally_linked_input = node_internally_linked_input;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_xpbd_physics_solver_cc
