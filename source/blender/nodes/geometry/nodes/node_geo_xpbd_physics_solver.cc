/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances.hh"

#include "BLI_array_utils.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"

#include "BLI_ordered_edge.hh"
#include "DNA_mesh_types.h"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"

#include "GEO_xpbd_common_constraint_evaluators.hh"
#include "GEO_xpbd_common_constraint_set_indices.hh"
#include "GEO_xpbd_constraint_solver.hh"

#include "intern/attribute_storage_access.hh"
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
  Array<float3> positions;
  Array<float3> velocities;
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

static void apply_external_accelerations_and_velocities(
    XPBDState &state,
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

static GeometrySet apply_simulation(const XPBDGeometryBundle &bundle, const XPBDState &state)
{
  GeometrySet geometry = bundle.geometry;

  if (geometry.has_mesh()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Mesh};
    if (const SimPoints *sim_points = state.sim_points.lookup_ptr(key)) {
      apply_simulation_to_mesh(geometry, *sim_points);
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
    const Map<SimPointsKey, Span<float>> inverse_masses_map,
    const Span<GeometrySet> applied_geometries)
{
  float3 gravity(0.0f);
  for (const GravityBundle &gravity_bundle : world.gravities) {
    gravity = gravity_bundle.gravity;
  }
  Map<SimPointsKey, Span<float3>> accelerations_map;
  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];
    Vector<const ForceBundle *> used_forces = filter_bundles_for_path<ForceBundle>(
        world.forces, geometry_bundle.self_path);
    for (const bke::GeometryComponent::Type type : {bke::GeometryComponent::Type::Mesh}) {
      const bke::GeometryComponent *component = applied_geometry.get_component(type);
      if (!component) {
        continue;
      }
      const bke::AttrDomain domain = bke::AttrDomain::Point;
      const int domain_size = component->attribute_domain_size(domain);
      const Span<float> inverse_masses = inverse_masses_map.lookup(
          {geometry_bundle.self_path, type});

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

      accelerations_map.add_new({geometry_bundle.self_path, type}, result);
    }
  }
  return accelerations_map;
}

/**
 * Computes the inverse mass for each point. The inverse mass of points that are known to be pinned
 * is 0 (aka they are assumed to have infinite mass).
 */
static Map<SimPointsKey, Span<float>> compute_inverse_masses(
    ResourceScope &scope, const WorldData &world, const Span<GeometrySet> applied_geometries)
{
  Map<SimPointsKey, Span<float>> inverse_masses_map;
  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];

    const Vector<const PinnedPositionXPBDConstraintBundle *> pinned_position_constraints =
        filter_bundles_for_path<PinnedPositionXPBDConstraintBundle>(
            world.pinned_position_constraints, geometry_bundle.self_path);

    for (const bke::GeometryComponent::Type type : {bke::GeometryComponent::Type::Mesh}) {
      const bke::GeometryComponent *component = applied_geometry.get_component(type);
      if (!component) {
        continue;
      }
      const bke::AttrDomain domain = bke::AttrDomain::Point;
      const int domain_size = component->attribute_domain_size(domain);

      MutableSpan<float> result = scope.allocator().allocate_array<float>(domain_size);

      auto &field_context = scope.construct<bke::GeometryFieldContext>(*component, domain);
      auto &mass_evaluator = scope.construct<fn::FieldEvaluator>(field_context, domain_size);
      mass_evaluator.add_with_destination(geometry_bundle.mass, result);
      mass_evaluator.evaluate();

      /* Invert masses. */
      threading::parallel_for(IndexRange(domain_size), 1024, [&](const IndexRange range) {
        for (const int i : range) {
          result[i] = 1.0f / result[i];
        }
      });

      for (const PinnedPositionXPBDConstraintBundle *constraint : pinned_position_constraints) {
        fn::FieldEvaluator pin_evaluator{field_context, domain_size};
        pin_evaluator.set_selection(constraint->selection);
        pin_evaluator.evaluate();
        const IndexMask mask = pin_evaluator.get_evaluated_selection_as_mask();
        if (!mask.is_empty()) {
          mask.foreach_index(GrainSize(1024), [&](const int i) { result[i] = 0.0f; });
        }
      }
      inverse_masses_map.add_new({geometry_bundle.self_path, type}, result);
    }
  }
  return inverse_masses_map;
}

static void gather_distance_constraints(
    ResourceScope &scope,
    XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &all_sim_points_keys,
    const Span<SimPoints *> all_sim_points,
    const Map<SimPointsKey, Span<float>> &inverse_masses_map,
    const float delta_time,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];
    if (!applied_geometry.has_mesh()) {
      continue;
    }
    const SimPointsKey component_key = {geometry_bundle.self_path,
                                        bke::GeometryComponent::Type::Mesh};
    const int geo_i = all_sim_points_keys.index_of(component_key);
    const SimPoints &sim_points = *all_sim_points[geo_i];
    const Mesh &mesh = *applied_geometry.get_mesh();
    const Span<float3> mesh_positions = mesh.vert_positions();
    const Span<int2> mesh_edges = mesh.edges();
    const Span<float> inverse_masses = inverse_masses_map.lookup(component_key);

    const Vector<const EdgeLengthXPBDConstraintBundle *> edge_length_constraints =
        filter_bundles_for_path<EdgeLengthXPBDConstraintBundle>(world.edge_length_constraints,
                                                                geometry_bundle.self_path);
    bke::MeshFieldContext field_context(mesh, bke::AttrDomain::Edge);
    for (const EdgeLengthXPBDConstraintBundle *constraint_bundle : edge_length_constraints) {
      fn::FieldEvaluator field_evaluator{field_context, mesh_edges.size()};
      field_evaluator.set_selection(constraint_bundle->selection);
      field_evaluator.add(constraint_bundle->compliance);
      field_evaluator.evaluate();
      const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
      if (mask.is_empty()) {
        continue;
      }
      Span<int2> constraint_edges;
      if (mask.size() == mesh_edges.size()) {
        constraint_edges = mesh_edges;
      }
      else {
        MutableSpan<int2> masked_edges = scope.allocator().allocate_array<int2>(mask.size());
        array_utils::gather(mesh_edges, mask, masked_edges);
        constraint_edges = masked_edges;
      }

      MutableSpan<float> compliance_terms = scope.allocator().allocate_array<float>(mask.size());
      field_evaluator.get_evaluated<float>(0).materialize_compressed(mask, compliance_terms);

      MutableSpan<float> constraint_lengths = scope.allocator().allocate_array<float>(mask.size());
      DistanceConstraintLengths &distance_constraint_lengths =
          state.distance_constraint_lengths.lookup_or_add_default(component_key);
      threading::parallel_for(constraint_edges.index_range(), 512, [&](const IndexRange range) {
        for (const int i : range) {
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
      });

      const float compliance_factor = math::safe_divide(1.0f, pow2f(delta_time));
      threading::parallel_for(mask.index_range(), 512, [&](const IndexRange range) {
        for (float &compliance_term : compliance_terms.slice(range)) {
          compliance_term *= compliance_factor;
        }
      });

      r_constraint_sets.append(
          {scope.construct<geometry::xpbd_constraint_solver::BinaryConstraintSetIndices>(
               geo_i, constraint_edges),
           scope.construct<geometry::xpbd_constraint_solver::DistanceConstraintEvaluator>(
               geo_i,
               sim_points.positions,
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
    const VectorSet<SimPointsKey> &ordered_sim_points_keys,
    const Span<SimPoints *> ordered_sim_points,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];
    const Vector<const PinnedPositionXPBDConstraintBundle *> pinned_position_constraints =
        filter_bundles_for_path<PinnedPositionXPBDConstraintBundle>(
            world.pinned_position_constraints, geometry_bundle.self_path);
    if (pinned_position_constraints.is_empty()) {
      continue;
    }
    for (const bke::GeometryComponent::Type type : {bke::GeometryComponent::Type::Mesh}) {
      const bke::GeometryComponent *component = applied_geometry.get_component(type);
      if (!component) {
        continue;
      }
      const AttrDomain domain = bke::AttrDomain::Point;
      const int domain_size = component->attribute_domain_size(domain);

      const int geo_i = ordered_sim_points_keys.index_of({geometry_bundle.self_path, type});
      const SimPoints &sim_points = *ordered_sim_points[geo_i];

      bke::GeometryFieldContext field_context(*component, domain);
      for (const PinnedPositionXPBDConstraintBundle *constraint_bundle :
           pinned_position_constraints)
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
        r_constraint_sets.append(
            {scope.construct<geometry::xpbd_constraint_solver::UnaryConstraintSetIndices>(
                 geo_i, constraint_indices),
             scope.construct<geometry::xpbd_constraint_solver::PinConstraintEvaluator>(
                 geo_i, sim_points.positions, constraint_indices, constraint_positions)});
      }
    }
  }
}

static void update_and_step_xpbd_state(XPBDState &state,
                                       const WorldData &world,
                                       const float total_delta_time,
                                       const SolverType solver_type,
                                       const int substeps)
{
  ResourceScope scope;
  Array<GeometrySet> applied_geometries(world.geometries.size());

  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    GeometrySet &applied_geometry = applied_geometries[bundle_i];
    applied_geometry = apply_simulation(geometry_bundle, state);
  }

  Map<SimPointsKey, SimPoints> new_sim_points;
  for (const int bundle_i : world.geometries.index_range()) {
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];
    update_xpbd_state_for_geometry(
        state, world.geometries[bundle_i], applied_geometry, new_sim_points);
  }
  state.sim_points = std::move(new_sim_points);

  VectorSet<SimPointsKey> ordered_sim_points_keys;
  Vector<SimPoints *> ordered_sim_points;
  Vector<geometry::xpbd_constraint_solver::PointsRef> points_refs;
  for (auto item : state.sim_points.items()) {
    SimPoints &sim_points = item.value;
    ordered_sim_points_keys.add_new(item.key);
    ordered_sim_points.append(&sim_points);
    points_refs.append({sim_points.positions});
  }

  for (DistanceConstraintLengths &distance_constraint_lengths :
       state.distance_constraint_lengths.values())
  {
    for (DistanceConstraintLengths::LengthItem &length_item :
         distance_constraint_lengths.lengths.values())
    {
      length_item.used = false;
    }
  }

  const Map<SimPointsKey, Span<float>> inverse_masses_map = compute_inverse_masses(
      scope, world, applied_geometries);

  const Map<SimPointsKey, Span<float3>> accelerations_map = compute_external_accelerations(
      scope, world, inverse_masses_map, applied_geometries);

  const float sub_delta_time = math::safe_divide<float>(total_delta_time, substeps);

  Vector<geometry::xpbd_constraint_solver::ConstraintSet> constraint_sets;
  gather_distance_constraints(scope,
                              state,
                              world,
                              applied_geometries,
                              ordered_sim_points_keys,
                              ordered_sim_points,
                              inverse_masses_map,
                              sub_delta_time,
                              constraint_sets);
  gather_pin_constraints(scope,
                         world,
                         applied_geometries,
                         ordered_sim_points_keys,
                         ordered_sim_points,
                         constraint_sets);

  Array<Array<float3>> all_prev_positions(ordered_sim_points.size());
  for (const int i : ordered_sim_points.index_range()) {
    all_prev_positions[i].reinitialize(ordered_sim_points[i]->points_num);
  }

  for ([[maybe_unused]] const int substep_i : IndexRange(substeps)) {
    for (const int i : ordered_sim_points.index_range()) {
      all_prev_positions[i].as_mutable_span().copy_from(ordered_sim_points[i]->positions);
    }
    if (sub_delta_time > 0.0f) {
      apply_external_accelerations_and_velocities(state, accelerations_map, sub_delta_time);
    }

    switch (solver_type) {
      case SolverType::SerialGaussSeidel: {
        geometry::xpbd_constraint_solver::solve_gauss_seidel_one_at_a_time(points_refs,
                                                                           constraint_sets);
        break;
      }
      case SolverType::ParallelGaussSeidel: {
        geometry::xpbd_constraint_solver::solve_gauss_seidel_parallel(points_refs,
                                                                      constraint_sets);
        break;
      }
      case SolverType::NonDeterministicJacobian: {
        geometry::xpbd_constraint_solver::solve_jacobian_non_deterministic(points_refs,
                                                                           constraint_sets);
        break;
      }
    }

    if (sub_delta_time > 0.0f) {
      for (const int geo_i : ordered_sim_points.index_range()) {
        Span<float3> prev_positions = all_prev_positions[geo_i];
        SimPoints &sim_points = *ordered_sim_points[geo_i];
        Span<float3> new_positions = sim_points.positions;
        MutableSpan<float3> velocities = sim_points.velocities;
        threading::parallel_for(
            IndexRange(sim_points.points_num), 1024, [&](const IndexRange range) {
              for (const int i : range) {
                const float3 &prev_position = prev_positions[i];
                const float3 &new_position = new_positions[i];
                const float3 velocity = (new_position - prev_position) / sub_delta_time;
                velocities[i] = velocity;
              }
            });
      }
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
