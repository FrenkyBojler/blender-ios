/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_instances.hh"

#include "BLI_array_utils.hh"
#include "BLI_kdtree.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"
#include "BLI_ordered_edge.hh"

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"

#include "GEO_xpbd_common_constraint_evaluators.hh"
#include "GEO_xpbd_common_constraint_set_indices.hh"
#include "GEO_xpbd_constraint_solver.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_xpbd_physics_solver_cc {

using namespace physics_bundles;

enum class SolverType {
  SerialGaussSeidel,
  ParallelGaussSeidel,
  NonDeterministicJacobian,
};

static const EnumPropertyItem solver_type_items[] = {
    {int(SolverType::SerialGaussSeidel), "SERIAL_GAUSS_SEIDEL", 0, "Serial Gauss-Seidel", ""},
    {int(SolverType::ParallelGaussSeidel),
     "PARALLEL_GAUSS_SEIDEL",
     0,
     "Parallel Gauss-Seidel",
     ""},
    {int(SolverType::NonDeterministicJacobian),
     "NON_DETERMINISTIC_JACOBIAN",
     0,
     "Non-deterministic Jacobian",
     ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static NestedBundleTypePtr make_world_type()
{
  Vector<std::shared_ptr<const FlatBundleType>> types;
  types.append(GravityBundle::get_bundle_type());
  types.append(ForceBundle::get_bundle_type());
  types.append(XPBDGeometryBundle::get_bundle_type());
  types.append(EdgeLengthXPBDConstraintBundle::get_bundle_type());
  types.append(PinnedPositionXPBDConstraintBundle::get_bundle_type());
  types.append(InfiniteGroundPlaneBundle::get_bundle_type());

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

  auto &panel = b.add_panel("Solver");
  panel.add_input<decl::Menu>("Solver Type").static_items(solver_type_items);
  panel.add_input<decl::Int>("Substeps").default_value(1).min(1);
}

struct SimPoints {
  int points_num;
  bool has_rotation = false;

  Array<float3> positions;
  Array<float3> velocities;

  Array<math::Quaternion> rotations;
  Array<float3> angular_velocities;
};

struct SimPointsKey {
  std::string path;
  bke::GeometryComponent::Type type;

  BLI_STRUCT_EQUALITY_OPERATORS_2(SimPointsKey, path, type)

  uint64_t hash() const
  {
    return get_default_hash(this->path, this->type);
  }
};

/**
 * Properties of simulated points which are retrieved from the world instead of being stored in
 * the state.
 */
struct SimPointsWorldProperties {
  Span<float> inverse_masses;
  Span<float> frictions;
};

struct DistanceConstraintLengths {
  struct LengthItem {
    float length;
    bool used = true;
  };

  Map<OrderedEdge, LengthItem> lengths;
};

class XPBDState {
 public:
  /**
   * Counts how often this state has been updated. This is mainly used to avoid re-simulating the
   * same frame multiple times when playback is paused but the simulation parameters are changed.
   */
  int update_counter = 0;

  Map<SimPointsKey, SimPoints> sim_points;
  Map<SimPointsKey, DistanceConstraintLengths> distance_constraint_lengths;
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
  struct SelfPathGetter {
    StringRefNull operator()(const NestedBundleCommon &bundle)
    {
      return bundle.self_path;
    }
  };
  template<typename T> using BundleVectorSet = CustomIDVectorSet<T, SelfPathGetter>;

  BundleVectorSet<ForceBundle> forces;
  BundleVectorSet<GravityBundle> gravities;
  BundleVectorSet<XPBDGeometryBundle> geometries;
  BundleVectorSet<EdgeLengthXPBDConstraintBundle> edge_length_constraints;
  BundleVectorSet<PinnedPositionXPBDConstraintBundle> pinned_position_constraints;
  BundleVectorSet<InfiniteGroundPlaneBundle> infinite_ground_planes;
};

static AttrDomain get_position_domain(const bke::GeometryComponent::Type type)
{
  return type == bke::GeometryComponent::Type::Instance ? AttrDomain::Instance : AttrDomain::Point;
}

static WorldData parse_world(const Bundle &world_bundle)
{
  WorldData world;
  nested_bundle_foreach(world_bundle, [&](HandleNestedBundleParams &params) {
    BundleParseErrors errors;
    if (params.type == ForceBundle::name) {
      if (std::optional<ForceBundle> force = ForceBundle::parse(params.bundle, errors)) {
        force->self_path = Bundle::combine_path(params.path);
        world.forces.add_new(std::move(*force));
      }
    }
    else if (params.type == GravityBundle::name) {
      if (std::optional<GravityBundle> gravity = GravityBundle::parse(params.bundle, errors)) {
        gravity->self_path = Bundle::combine_path(params.path);
        world.gravities.add_new(std::move(*gravity));
      }
    }
    else if (params.type == XPBDGeometryBundle::name) {
      if (std::optional<XPBDGeometryBundle> geometry = XPBDGeometryBundle::parse(params.bundle,
                                                                                 errors))
      {
        geometry->self_path = Bundle::combine_path(params.path);
        world.geometries.add_new(std::move(*geometry));
      }
    }
    else if (params.type == EdgeLengthXPBDConstraintBundle::name) {
      if (std::optional<EdgeLengthXPBDConstraintBundle> constraint =
              EdgeLengthXPBDConstraintBundle::parse(params.bundle, errors))
      {
        constraint->self_path = Bundle::combine_path(params.path);
        world.edge_length_constraints.add_new(std::move(*constraint));
      }
    }
    else if (params.type == PinnedPositionXPBDConstraintBundle::name) {
      if (std::optional<PinnedPositionXPBDConstraintBundle> constraint =
              PinnedPositionXPBDConstraintBundle::parse(params.bundle, errors))
      {
        constraint->self_path = Bundle::combine_path(params.path);
        world.pinned_position_constraints.add_new(std::move(*constraint));
      }
    }
    else if (params.type == InfiniteGroundPlaneBundle::name) {
      if (std::optional<InfiniteGroundPlaneBundle> constraint = InfiniteGroundPlaneBundle::parse(
              params.bundle, errors))
      {
        constraint->self_path = Bundle::combine_path(params.path);
        world.infinite_ground_planes.add_new(std::move(*constraint));
      }
    }
  });
  return world;
}

static void integrate_velocities(XPBDState &state,
                                 const Map<SimPointsKey, Span<float3>> &accelerations_map,
                                 const float delta_time)
{
  for (auto item : state.sim_points.items()) {
    SimPoints &sim_points = item.value;
    const Span<float3> accelerations = accelerations_map.lookup(item.key);
    const int points_num = sim_points.positions.size();
    threading::parallel_for(IndexRange(points_num), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        const float3 &acceleration = accelerations[i];
        sim_points.velocities[i] += acceleration * delta_time;
        sim_points.positions[i] += sim_points.velocities[i] * delta_time;
      }
    });
    if (sim_points.has_rotation) {
      threading::parallel_for(IndexRange(points_num), 1024, [&](const IndexRange range) {
        for (const int i : range) {
          /* There are no "torque fields" yet. */
          const float3 external_torque(0.0f, 0.0f, 0.0f);
          /* TODO: Support customizable inertia. */
          const float3 inertia(1.0f);
          float3 &angular_velocity = sim_points.angular_velocities[i];
          const float3 precession = math::cross(angular_velocity, angular_velocity * inertia);
          angular_velocity += delta_time *
                              math::safe_divide(external_torque - precession, inertia);
          math::Quaternion &rotation = sim_points.rotations[i];
          const math::Quaternion direction = rotation * math::Quaternion(0, angular_velocity);
          rotation = math::normalize(
              math::Quaternion(float4(rotation) + delta_time * 0.5f * float4(direction)));
        }
      });
    }
  }
}

static void apply_simulation_to_mesh(GeometrySet &geometry, const SimPoints &sim_points)
{
  if (!geometry.has_mesh()) {
    return;
  }
  if (geometry.get_mesh()->verts_num != sim_points.points_num) {
    return;
  }
  Mesh *mesh = geometry.get_mesh_for_write();
  MutableSpan<float3> mesh_positions = mesh->vert_positions_for_write();
  mesh_positions.copy_from(sim_points.positions);
  mesh->tag_positions_changed();
}

static void apply_simulation_to_pointcloud(GeometrySet &geometry, const SimPoints &sim_points)
{
  if (!geometry.has_pointcloud()) {
    return;
  }
  if (geometry.get_pointcloud()->totpoint != sim_points.points_num) {
    return;
  }
  PointCloud *pointcloud = geometry.get_pointcloud_for_write();
  MutableSpan<float3> pointcloud_positions = pointcloud->positions_for_write();
  pointcloud_positions.copy_from(sim_points.positions);
  pointcloud->tag_positions_changed();
}

static void apply_simulation_to_curves(GeometrySet &geometry, const SimPoints &sim_points)
{
  if (!geometry.has_curves()) {
    return;
  }
  if (geometry.get_curves()->geometry.wrap().points_num() != sim_points.points_num) {
    return;
  }
  Curves *curves_id = geometry.get_curves_for_write();
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  MutableSpan<float3> curves_positions = curves.positions_for_write();
  curves_positions.copy_from(sim_points.positions);
  curves.tag_positions_changed();
}

static void apply_simulation_to_instances(GeometrySet &geometry, const SimPoints &sim_points)
{
  if (!geometry.has_instances()) {
    return;
  }
  if (geometry.get_instances()->instances_num() != sim_points.points_num) {
    return;
  }
  bke::Instances *instances = geometry.get_instances_for_write();
  MutableSpan<float4x4> transforms = instances->transforms_for_write();
  const Span<float3> positions = sim_points.positions;
  if (sim_points.has_rotation) {
    const Span<math::Quaternion> rotations = sim_points.rotations;
    threading::parallel_for(transforms.index_range(), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        float4x4 &transform = transforms[i];
        const float3 scale = math::to_scale<true>(transform);
        transform = math::from_loc_rot_scale<float4x4>(positions[i], rotations[i], scale);
      }
    });
  }
  else {
    threading::parallel_for(transforms.index_range(), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        transforms[i].location() = positions[i];
      }
    });
  }
}

static GeometrySet apply_simulation(const XPBDGeometryBundle &bundle, const XPBDState &state)
{
  GeometrySet geometry = bundle.geometry;

  if (geometry.has_mesh()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Mesh};
    if (const SimPoints *sim_points = state.sim_points.lookup_ptr(key)) {
      apply_simulation_to_mesh(geometry, *sim_points);
    }
  }
  if (geometry.has_pointcloud()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::PointCloud};
    if (const SimPoints *sim_points = state.sim_points.lookup_ptr(key)) {
      apply_simulation_to_pointcloud(geometry, *sim_points);
    }
  }
  if (geometry.has_curves()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Curve};
    if (const SimPoints *sim_points = state.sim_points.lookup_ptr(key)) {
      apply_simulation_to_curves(geometry, *sim_points);
    }
  }
  if (geometry.has_instances()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Instance};
    if (const SimPoints *sim_points = state.sim_points.lookup_ptr(key)) {
      apply_simulation_to_instances(geometry, *sim_points);
    }
  }

  return geometry;
}

static void update_xpbd_state_for_geometry(XPBDState &state,
                                           const XPBDGeometryBundle &bundle,
                                           const GeometrySet &current_geometry,
                                           Map<SimPointsKey, SimPoints> &r_sim_points)
{
  if (current_geometry.has_mesh()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Mesh};
    const Mesh *current_mesh = current_geometry.get_mesh();
    std::optional<SimPoints> new_sim_points;
    if (std::optional<SimPoints> old_sim_points = state.sim_points.pop_try(key)) {
      if (current_mesh->verts_num == old_sim_points->points_num) {
        new_sim_points = std::move(*old_sim_points);
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
    r_sim_points.add(key, std::move(*new_sim_points));
  }
  if (current_geometry.has_pointcloud()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::PointCloud};
    const PointCloud *current_pointcloud = current_geometry.get_pointcloud();
    std::optional<SimPoints> new_sim_points;
    if (std::optional<SimPoints> old_sim_points = state.sim_points.pop_try(key)) {
      if (current_pointcloud->totpoint == old_sim_points->points_num) {
        new_sim_points = std::move(*old_sim_points);
      }
    }
    if (!new_sim_points) {
      SimPoints sim_points;
      sim_points.points_num = current_pointcloud->totpoint;
      sim_points.positions = current_pointcloud->positions();
      sim_points.velocities.reinitialize(current_pointcloud->totpoint);
      sim_points.velocities.fill(float3(0.0f));
      new_sim_points = std::move(sim_points);
    }
    r_sim_points.add(key, std::move(*new_sim_points));
  }
  if (current_geometry.has_curves()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Curve};
    const Curves *current_curves_id = current_geometry.get_curves();
    const bke::CurvesGeometry &current_curves = current_curves_id->geometry.wrap();
    std::optional<SimPoints> new_sim_points;
    if (std::optional<SimPoints> old_sim_points = state.sim_points.pop_try(key)) {
      if (current_curves.points_num() == old_sim_points->points_num) {
        new_sim_points = std::move(*old_sim_points);
      }
    }
    if (!new_sim_points) {
      SimPoints sim_points;
      sim_points.points_num = current_curves.points_num();
      sim_points.positions = current_curves.positions();
      sim_points.velocities.reinitialize(current_curves.points_num());
      sim_points.velocities.fill(float3(0.0f));
      new_sim_points = std::move(sim_points);
    }
    r_sim_points.add(key, std::move(*new_sim_points));
  }
  if (current_geometry.has_instances()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Instance};
    const bke::Instances *current_instances = current_geometry.get_instances();
    std::optional<SimPoints> new_sim_points;
    if (std::optional<SimPoints> old_sim_points = state.sim_points.pop_try(key)) {
      if (current_instances->instances_num() == old_sim_points->points_num) {
        new_sim_points = std::move(*old_sim_points);
      }
    }
    if (!new_sim_points) {
      SimPoints sim_points;
      const int instances_num = current_instances->instances_num();
      sim_points.points_num = instances_num;
      sim_points.has_rotation = true;
      sim_points.positions.reinitialize(instances_num);
      sim_points.rotations.reinitialize(instances_num);
      const Span<float4x4> transforms = current_instances->transforms();
      threading::parallel_for(
          sim_points.positions.index_range(), 1024, [&](const IndexRange range) {
            for (const int i : range) {
              float3 location;
              math::Quaternion rotation;
              float3 scale;
              math::to_loc_rot_scale_safe<true>(transforms[i], location, rotation, scale);

              sim_points.positions[i] = location;
              sim_points.rotations[i] = rotation;
            }
          });
      sim_points.velocities.reinitialize(instances_num);
      sim_points.velocities.fill(float3(0.0f));
      sim_points.angular_velocities.reinitialize(instances_num);
      sim_points.angular_velocities.fill(float3(0.0f));

      new_sim_points = std::move(sim_points);
    }
    r_sim_points.add(key, std::move(*new_sim_points));
  }
}

template<typename T>
static Vector<const T *> filter_bundles_for_path(const Span<T> bundles, const StringRef path)
{
  static_assert(std::is_base_of_v<NestedBundleCommon, T>);
  Vector<const T *> used_forces;
  for (const T &bundle : bundles) {
    if (nested_bundle_path_is_selected(bundle.self_path, bundle.filter, path)) {
      used_forces.append(&bundle);
    }
  }
  return used_forces;
}

static Map<SimPointsKey, Span<float3>> compute_external_accelerations(
    ResourceScope &scope,
    const WorldData &world,
    const Span<SimPointsKey> keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const Span<GeometrySet> applied_geometries)
{
  float3 gravity(0.0f);
  for (const GravityBundle &gravity_bundle : world.gravities) {
    gravity = gravity_bundle.gravity;
  }
  Map<SimPointsKey, Span<float3>> accelerations_map;
  for (const SimPointsKey &sim_points_key : keys) {
    const int geometry_i = world.geometries.index_of_as(sim_points_key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_i];
    Vector<const ForceBundle *> used_forces = filter_bundles_for_path<ForceBundle>(
        world.forces, sim_points_key.path);
    const bke::GeometryComponent::Type type = sim_points_key.type;
    const bke::GeometryComponent *component = applied_geometry.get_component(type);
    if (!component) {
      continue;
    }
    const bke::AttrDomain domain = get_position_domain(type);
    const int domain_size = component->attribute_domain_size(domain);
    const Span<float> inverse_masses = sim_points_props.lookup(sim_points_key).inverse_masses;

    /* Initially this is the sums of forces and then the acceleration. */
    MutableSpan<float3> result = scope.allocator().allocate_array<float3>(domain_size);
    result.fill(float3(0.0f));

    bke::GeometryFieldContext field_context(*component, domain);
    for (const ForceBundle *force_bundle : used_forces) {
      fn::FieldEvaluator field_evaluator{field_context, domain_size};
      field_evaluator.set_selection(force_bundle->selection);
      field_evaluator.add(force_bundle->force);
      field_evaluator.evaluate();
      const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
      const VArray<float3> force = field_evaluator.get_evaluated<float3>(0);
      mask.foreach_index([&](const int i) { result[i] += force[i]; });
    }

    threading::parallel_for(IndexRange(domain_size), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        const float inverse_mass = inverse_masses[i];
        const float3 &force = result[i];
        const float3 acceleration = force * inverse_mass + gravity;
        result[i] = acceleration;
      }
    });

    accelerations_map.add_new(sim_points_key, result);
  }
  return accelerations_map;
}

static Map<SimPointsKey, SimPointsWorldProperties> compute_sim_point_world_properties(
    ResourceScope &scope,
    const WorldData &world,
    const Span<SimPointsKey> keys,
    const Span<GeometrySet> applied_geometries)
{
  Map<SimPointsKey, SimPointsWorldProperties> properties_map;
  for (const SimPointsKey &key : keys) {
    const int geometry_i = world.geometries.index_of_as(key.path);
    const XPBDGeometryBundle &geometry_bundle = world.geometries[geometry_i];
    const GeometrySet &applied_geometry = applied_geometries[geometry_i];

    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometry.get_component(type);
    if (!component) {
      continue;
    }

    const bke::AttrDomain domain = get_position_domain(type);
    const int domain_size = component->attribute_domain_size(domain);

    MutableSpan<float> result_masses = scope.allocator().allocate_array<float>(domain_size);
    MutableSpan<float> result_frictions = scope.allocator().allocate_array<float>(domain_size);

    auto &field_context = scope.construct<bke::GeometryFieldContext>(*component, domain);
    auto &field_evaluator = scope.construct<fn::FieldEvaluator>(field_context, domain_size);
    field_evaluator.add_with_destination(geometry_bundle.mass, result_masses);
    field_evaluator.add_with_destination(geometry_bundle.friction, result_frictions);
    field_evaluator.evaluate();

    /* Invert masses and clamp frictions. */
    threading::parallel_for(IndexRange(domain_size), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        result_masses[i] = std::max(0.0f, math::safe_divide(1.0f, result_masses[i]));
        result_frictions[i] = std::max(0.0f, result_frictions[i]);
      }
    });

    /* Set mass of pinned points to infinity (i.e. the inverse mass is 0). */
    const Vector<const PinnedPositionXPBDConstraintBundle *> pinned_position_constraints =
        filter_bundles_for_path<PinnedPositionXPBDConstraintBundle>(
            world.pinned_position_constraints, key.path);
    for (const PinnedPositionXPBDConstraintBundle *constraint : pinned_position_constraints) {
      fn::FieldEvaluator pin_evaluator{field_context, domain_size};
      pin_evaluator.set_selection(constraint->selection);
      pin_evaluator.evaluate();
      const IndexMask mask = pin_evaluator.get_evaluated_selection_as_mask();
      if (!mask.is_empty()) {
        mask.foreach_index(GrainSize(1024), [&](const int i) { result_masses[i] = 0.0f; });
      }
    }
    properties_map.add_new(key, {result_masses, result_frictions});
  }
  return properties_map;
}

static void gather_edge_length_constraints(
    ResourceScope &scope,
    XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const float delta_time,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Mesh) {
      continue;
    }
    const int geometry_bundle_i = world.geometries.index_of_as(key.path);
    const XPBDGeometryBundle &geometry_bundle = world.geometries[geometry_bundle_i];
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];

    const Mesh &mesh = *applied_geometry.get_mesh();
    const Span<float3> mesh_positions = mesh.vert_positions();
    const Span<int2> mesh_edges = mesh.edges();
    const Span<float> inverse_masses = sim_points_props.lookup(key).inverse_masses;

    const Vector edge_length_constraints = filter_bundles_for_path<EdgeLengthXPBDConstraintBundle>(
        world.edge_length_constraints, geometry_bundle.self_path);

    bke::MeshFieldContext edge_field_context(mesh, bke::AttrDomain::Edge);
    for (const EdgeLengthXPBDConstraintBundle *constraint_bundle : edge_length_constraints) {
      fn::FieldEvaluator field_evaluator{edge_field_context, mesh_edges.size()};
      field_evaluator.set_selection(constraint_bundle->selection);
      field_evaluator.add(constraint_bundle->compliance);
      field_evaluator.evaluate();
      const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
      if (mask.is_empty()) {
        continue;
      }

      /* Gather selected constraint edges. */
      Span<int2> constraint_edges;
      if (mask.size() == mesh_edges.size()) {
        constraint_edges = mesh_edges;
      }
      else {
        MutableSpan<int2> masked_edges = scope.allocator().allocate_array<int2>(mask.size());
        array_utils::gather(mesh_edges, mask, masked_edges);
        constraint_edges = masked_edges;
      }

      /* Prepare per-constraint compliance. */
      MutableSpan<float> compliance_terms = scope.allocator().allocate_array<float>(mask.size());
      field_evaluator.get_evaluated<float>(0).materialize_compressed(mask, compliance_terms);
      const float compliance_factor = math::safe_divide(1.0f, pow2f(delta_time));
      threading::parallel_for(mask.index_range(), 512, [&](const IndexRange range) {
        for (float &compliance_term : compliance_terms.slice(range)) {
          compliance_term *= compliance_factor;
        }
      });

      /* Prepare per-constraint length. */
      MutableSpan<float> constraint_lengths = scope.allocator().allocate_array<float>(mask.size());
      DistanceConstraintLengths &distance_constraint_lengths =
          state.distance_constraint_lengths.lookup_or_add_default(key);
      for (const int i : constraint_edges.index_range()) {
        const int2 &edge = constraint_edges[i];
        const OrderedEdge ordered_edge{edge[0], edge[1]};
        DistanceConstraintLengths::LengthItem &length_item =
            distance_constraint_lengths.lengths.lookup_or_add_cb(ordered_edge, [&]() {
              const float3 &p0 = mesh_positions[edge[0]];
              const float3 &p1 = mesh_positions[edge[1]];
              return DistanceConstraintLengths::LengthItem{math::distance(p0, p1)};
            });
        length_item.used = true;
        constraint_lengths[i] = length_item.length;
      }

      /* Add the actual constraint. */
      r_constraint_sets.append(
          {scope.construct<geometry::xpbd_constraint_solver::BinaryConstraintSetIndices>(
               key_i, constraint_edges),
           scope.construct<geometry::xpbd_constraint_solver::DistanceConstraintEvaluator>(
               scope.allocator().construct_array<int2>(constraint_edges.size(),
                                                       int2(key_i, key_i)),
               inverse_masses,
               constraint_edges,
               constraint_lengths,
               compliance_terms)});
    }
  }
}

static void gather_pin_constraints(
    ResourceScope &scope,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world.geometries.index_of_as(key.path);
    const XPBDGeometryBundle &geometry_bundle = world.geometries[geometry_bundle_i];
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];

    const Vector pinned_position_constraints =
        filter_bundles_for_path<PinnedPositionXPBDConstraintBundle>(
            world.pinned_position_constraints, geometry_bundle.self_path);
    if (pinned_position_constraints.is_empty()) {
      continue;
    }

    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometry.get_component(type);
    if (!component) {
      continue;
    }
    const AttrDomain domain = get_position_domain(type);
    const int domain_size = component->attribute_domain_size(domain);

    bke::GeometryFieldContext field_context(*component, domain);
    for (const PinnedPositionXPBDConstraintBundle *constraint_bundle : pinned_position_constraints)
    {
      fn::FieldEvaluator field_evaluator{field_context, domain_size};
      field_evaluator.set_selection(constraint_bundle->selection);
      field_evaluator.add(constraint_bundle->position);
      field_evaluator.evaluate();
      const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
      if (mask.is_empty()) {
        continue;
      }
      const VArray<float3> pin_positions_varray = field_evaluator.get_evaluated<float3>(0);
      MutableSpan<int> constraint_indices = scope.allocator().allocate_array<int>(mask.size());
      MutableSpan<float3> constraint_positions = scope.allocator().allocate_array<float3>(
          mask.size());
      mask.to_indices(constraint_indices);
      pin_positions_varray.materialize_compressed(mask, constraint_positions);

      /* Add actual constraint. */
      r_constraint_sets.append(
          {scope.construct<geometry::xpbd_constraint_solver::UnaryConstraintSetIndices>(
               key_i, constraint_indices),
           scope.construct<geometry::xpbd_constraint_solver::PinConstraintEvaluator>(
               scope.allocator().construct_array<int>(constraint_indices.size(), key_i),
               constraint_indices,
               constraint_positions)});
    }
  }
}

struct StaticPlaneContacts {
  Vector<int> indices;
  Vector<float3> plane_positions;
  Vector<float3> plane_normals;
  Vector<float> static_frictions;
  Vector<float> dynamic_frictions;
  Vector<float> depths;
};

struct DynamicSphereContacts {
  Vector<int2> indices;
  Vector<float> min_distance;
};

struct Contacts {
  Map<SimPointsKey, StaticPlaneContacts> static_plane_contacts;
  Map<SimPointsKey, DynamicSphereContacts> dynamic_sphere_contacts;
};

static void gather_ground_plane_contacts(const SimPoints &sim_points,
                                         const InfiniteGroundPlaneBundle &ground_plane,
                                         const Span<float> sim_points_frictions,
                                         StaticPlaneContacts &r_contacts)
{
  const float3 plane_normal = math::normalize(ground_plane.normal);
  if (math::is_zero(plane_normal)) {
    return;
  }
  for (const int point_i : IndexRange(sim_points.points_num)) {
    const float3 &position = sim_points.positions[point_i];
    const float distance = math::dot(position - ground_plane.position, plane_normal);
    if (distance >= 0.0f) {
      continue;
    }
    r_contacts.indices.append(point_i);
    r_contacts.plane_positions.append(ground_plane.position);
    r_contacts.plane_normals.append(plane_normal);
    const float point_friction = sim_points_frictions[point_i];
    const float friction = math::sqrt(point_friction * ground_plane.friction);
    r_contacts.static_frictions.append(friction);
    r_contacts.dynamic_frictions.append(friction);
    r_contacts.depths.append(-distance);
  }
}

static void gather_sphere_contacts(const SimPoints &sim_points,
                                   const Span<float> radii,
                                   DynamicSphereContacts &r_contacts)
{
  if (sim_points.points_num == 0) {
    return;
  }

  const float max_radius = *std::max_element(radii.begin(), radii.end());

  KDTree_3d *kdtree = BLI_kdtree_3d_new(sim_points.points_num);
  BLI_SCOPED_DEFER([&]() { BLI_kdtree_3d_free(kdtree); });

  for (const int i : sim_points.positions.index_range()) {
    BLI_kdtree_3d_insert(kdtree, i, sim_points.positions[i]);
  }
  BLI_kdtree_3d_balance(kdtree);

  for (const int i : sim_points.positions.index_range()) {
    const float3 &position = sim_points.positions[i];
    const float radius = radii[i];
    const float query_radius = radius + max_radius;
    BLI_kdtree_3d_range_search_cb_cpp(
        kdtree,
        position,
        query_radius,
        [&](const int other_i, const float * /*co*/, const float dist_sq) {
          if (i >= other_i) {
            return true;
          }
          const float other_radius = radii[other_i];
          const float min_distance = radius + other_radius;
          if (dist_sq >= pow2f(min_distance)) {
            return true;
          }
          r_contacts.indices.append({i, other_i});
          r_contacts.min_distance.append(min_distance);
          return true;
        });
  }
}

static Contacts gather_contacts(
    const XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const Span<SimPointsKey> keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props)
{
  Contacts contacts;
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world.geometries.index_of_as(key.path);
    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometries[geometry_bundle_i].get_component(
        type);
    const SimPoints &sim_points = state.sim_points.lookup(key);
    if (!component) {
      continue;
    }
    const Span<float> frictions = sim_points_props.lookup(key).frictions;

    {
      const Vector ground_plane_bundles = filter_bundles_for_path<InfiniteGroundPlaneBundle>(
          world.infinite_ground_planes, key.path);
      StaticPlaneContacts plane_contacts;
      for (const InfiniteGroundPlaneBundle *ground_plane_bundle : ground_plane_bundles) {
        gather_ground_plane_contacts(sim_points, *ground_plane_bundle, frictions, plane_contacts);
      }
      if (!plane_contacts.indices.is_empty()) {
        contacts.static_plane_contacts.add_new(key, std::move(plane_contacts));
      }
    }
    if (type == bke::GeometryComponent::Type::PointCloud) {
      const bke::PointCloudComponent &pointcloud_component =
          *static_cast<const bke::PointCloudComponent *>(component);
      if (const PointCloud *pointcloud = pointcloud_component.get()) {
        const VArraySpan<float> radii = pointcloud->radius();
        DynamicSphereContacts sphere_contacts;
        gather_sphere_contacts(sim_points, radii, sphere_contacts);
        if (!sphere_contacts.indices.is_empty()) {
          contacts.dynamic_sphere_contacts.add_new(key, std::move(sphere_contacts));
        }
      }
    }
  }
  return contacts;
}

static void generate_collision_constraint_sets(
    ResourceScope &scope,
    const Contacts &contacts,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const float delta_time,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  for (auto item : contacts.static_plane_contacts.items()) {
    const int key_i = keys.index_of(item.key);
    const StaticPlaneContacts &plane_contacts = item.value;
    r_constraint_sets.append(
        {scope.construct<geometry::xpbd_constraint_solver::UnaryConstraintSetIndices>(
             key_i, plane_contacts.indices),
         scope.construct<geometry::xpbd_constraint_solver::CollisionPlaneConstraintEvaluator>(
             scope.allocator().construct_array<int>(plane_contacts.indices.size(), key_i),
             plane_contacts.indices,
             plane_contacts.plane_positions,
             plane_contacts.plane_normals)});
  }
  for (auto item : contacts.dynamic_sphere_contacts.items()) {
    const int key_i = keys.index_of(item.key);
    const DynamicSphereContacts &sphere_contacts = item.value;
    const float compliance_term = math::safe_divide(1e-4f, pow2f(delta_time));
    r_constraint_sets.append(
        {scope.construct<geometry::xpbd_constraint_solver::BinaryConstraintSetIndices>(
             key_i, sphere_contacts.indices),
         scope.construct<geometry::xpbd_constraint_solver::MinimumDistanceConstraintEvaluator>(
             scope.allocator().construct_array<int2>(sphere_contacts.indices.size(),
                                                     int2(key_i, key_i)),
             sphere_contacts.indices,
             sphere_contacts.min_distance,
             sim_points_props.lookup(item.key).inverse_masses,
             scope.allocator().construct_array<float>(sphere_contacts.indices.size(),
                                                      compliance_term))});
  }
}

static Array<GeometrySet> gather_applied_geometries(const XPBDState &state, const WorldData &world)
{
  Array<GeometrySet> applied_geometries(world.geometries.size());

  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    GeometrySet &applied_geometry = applied_geometries[bundle_i];
    applied_geometry = apply_simulation(geometry_bundle, state);
  }
  return applied_geometries;
}

static void update_sim_points_from_world(XPBDState &state,
                                         const WorldData &world,
                                         const Span<GeometrySet> applied_geometries)
{
  Map<SimPointsKey, SimPoints> new_sim_points;
  for (const int bundle_i : world.geometries.index_range()) {
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];
    update_xpbd_state_for_geometry(
        state, world.geometries[bundle_i], applied_geometry, new_sim_points);
  }
  state.sim_points = std::move(new_sim_points);
}

static void reset_distance_constraint_length_usages(XPBDState &state)
{
  for (DistanceConstraintLengths &distance_constraint_lengths :
       state.distance_constraint_lengths.values())
  {
    for (DistanceConstraintLengths::LengthItem &length_item :
         distance_constraint_lengths.lengths.values())
    {
      length_item.used = false;
    }
  }
}

static void solve_constraints(
    const SolverType solver_type,
    const Span<geometry::xpbd_constraint_solver::MutablePointsRef> points_refs,
    const Span<geometry::xpbd_constraint_solver::ConstraintSet> constraint_sets)
{
  switch (solver_type) {
    case SolverType::SerialGaussSeidel: {
      geometry::xpbd_constraint_solver::solve_gauss_seidel_one_at_a_time(points_refs,
                                                                         constraint_sets);
      break;
    }
    case SolverType::ParallelGaussSeidel: {
      geometry::xpbd_constraint_solver::solve_gauss_seidel_parallel(points_refs, constraint_sets);
      break;
    }
    case SolverType::NonDeterministicJacobian: {
      geometry::xpbd_constraint_solver::solve_jacobian_non_deterministic(points_refs,
                                                                         constraint_sets);
      break;
    }
  }
}

static void apply_friction(XPBDState &state,
                           const Contacts &contacts,
                           const VectorSet<SimPointsKey> &keys,
                           const Span<Array<float3>> all_prev_positions)
{
  for (const auto item : contacts.static_plane_contacts.items()) {
    const int key_i = keys.index_of(item.key);
    SimPoints &sim_points = state.sim_points.lookup(item.key);
    const Span<float3> prev_positions = all_prev_positions[key_i];
    MutableSpan<float3> new_positions = sim_points.positions;
    const StaticPlaneContacts &plane_contacts = item.value;
    for (const int contact_i : plane_contacts.indices.index_range()) {
      const int point_i = plane_contacts.indices[contact_i];
      const float3 &plane_normal = plane_contacts.plane_normals[contact_i];
      const float static_friction = plane_contacts.static_frictions[contact_i];
      const float dynamic_friction = plane_contacts.dynamic_frictions[contact_i];
      const float depth = plane_contacts.depths[contact_i];
      const float3 pos_diff = new_positions[point_i] - prev_positions[point_i];
      const float3 tangential_pos_diff = pos_diff -
                                         plane_normal * math::dot(pos_diff, plane_normal);
      float3 offset = tangential_pos_diff;
      const float tangential_dist = math::length(tangential_pos_diff);
      if (tangential_dist >= static_friction * depth) {
        offset *= std::min(dynamic_friction * depth / tangential_dist, 1.0f);
      }

      new_positions[point_i] -= offset;
    }
  }
}

static void update_velocities(XPBDState &state,
                              const VectorSet<SimPointsKey> &keys,
                              const Span<Array<float3>> all_prev_positions,
                              const float delta_time)
{
  for (const int key_i : keys.index_range()) {
    const Span<float3> prev_positions = all_prev_positions[key_i];
    SimPoints &sim_points = state.sim_points.lookup(keys[key_i]);
    Span<float3> new_positions = sim_points.positions;
    MutableSpan<float3> velocities = sim_points.velocities;
    threading::parallel_for(IndexRange(sim_points.points_num), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        const float3 &prev_position = prev_positions[i];
        const float3 &new_position = new_positions[i];
        const float3 velocity = (new_position - prev_position) / delta_time;
        velocities[i] = velocity;
      }
    });
  }
}

static Vector<geometry::xpbd_constraint_solver::MutablePointsRef> prepare_points_refs_for_solver(
    XPBDState &state, const Span<SimPointsKey> keys)
{
  Vector<geometry::xpbd_constraint_solver::MutablePointsRef> points_refs;
  for (const SimPointsKey &key : keys) {
    SimPoints &sim_points = state.sim_points.lookup(key);
    geometry::xpbd_constraint_solver::MutablePointsRef points_ref;
    points_ref.positions = sim_points.positions;
    if (sim_points.has_rotation) {
      points_ref.rotations = sim_points.rotations;
    }
    points_refs.append(points_ref);
  }
  return points_refs;
}

static void update_and_step_xpbd_state(XPBDState &state,
                                       const WorldData &world,
                                       const float total_delta_time,
                                       const SolverType solver_type,
                                       const int substeps)
{
  ResourceScope scope;
  const Array<GeometrySet> applied_geometries = gather_applied_geometries(state, world);
  update_sim_points_from_world(state, world, applied_geometries);

  VectorSet<SimPointsKey> keys;
  for (const SimPointsKey &key : state.sim_points.keys()) {
    keys.add_new(key);
  }

  reset_distance_constraint_length_usages(state);

  const Map<SimPointsKey, SimPointsWorldProperties> sim_points_props =
      compute_sim_point_world_properties(scope, world, keys, applied_geometries);
  const Map<SimPointsKey, Span<float3>> accelerations_map = compute_external_accelerations(
      scope, world, keys, sim_points_props, applied_geometries);
  const float sub_delta_time = math::safe_divide<float>(total_delta_time, substeps);

  Vector<geometry::xpbd_constraint_solver::ConstraintSet> static_constraint_sets;
  gather_edge_length_constraints(scope,
                                 state,
                                 world,
                                 applied_geometries,
                                 keys,
                                 sim_points_props,
                                 sub_delta_time,
                                 static_constraint_sets);
  gather_pin_constraints(scope, world, applied_geometries, keys, static_constraint_sets);

  Array<Array<float3>> all_prev_positions(keys.size());
  for (const int i : keys.index_range()) {
    all_prev_positions[i].reinitialize(state.sim_points.lookup(keys[i]).points_num);
  }

  const Vector<geometry::xpbd_constraint_solver::MutablePointsRef> points_refs =
      prepare_points_refs_for_solver(state, keys);

  for ([[maybe_unused]] const int substep_i : IndexRange(substeps)) {
    /* Remember previous positions. */
    for (const int i : keys.index_range()) {
      const SimPointsKey &key = keys[i];
      all_prev_positions[i].as_mutable_span().copy_from(state.sim_points.lookup(key).positions);
    }

    /* Integrate linear and angular velocities. This also applies external forces. */
    if (sub_delta_time > 0.0f) {
      integrate_velocities(state, accelerations_map, sub_delta_time);
    }

    /* Find current collisisons and generate constraints to resolve them. */
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> constraint_sets =
        static_constraint_sets;
    const Contacts contacts = gather_contacts(
        state, world, applied_geometries, keys, sim_points_props);
    generate_collision_constraint_sets(
        scope, contacts, keys, sim_points_props, sub_delta_time, constraint_sets);

    /* Actually solve the constraints. */
    solve_constraints(solver_type, points_refs, constraint_sets);

    if (sub_delta_time > 0.0f) {
      /* Apply friction by updating current positions before the new velocity is computed. */
      apply_friction(state, contacts, keys, all_prev_positions);
      /* Update velocities based on previous and new positions. */
      update_velocities(state, keys, all_prev_positions, sub_delta_time);
    }
  }

  /* Remove unused distance constraint lengths. */
  for (DistanceConstraintLengths &distance_constraint_lengths :
       state.distance_constraint_lengths.values())
  {
    distance_constraint_lengths.lengths.remove_if(
        [](const auto &item) { return !item.value.used; });
  }
}

static void initialize_state(XPBDState & /*state*/)
{
  /* Nothing to do yet.*/
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr old_state_bundle_ptr = params.extract_input<BundlePtr>("State");
  BundlePtr world_bundle_ptr = params.extract_input<BundlePtr>("World");
  const SolverType solver_type = params.extract_input<SolverType>("Solver Type");
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
    update_and_step_xpbd_state(state, world, delta_time, solver_type, substeps);
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
  for (const XPBDGeometryBundle &bundle : world.geometries) {
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
