/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances.hh"

#include "BLI_array_utils.hh"
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

struct PathComponentKey {
  std::string path;
  bke::GeometryComponent::Type type;

  BLI_STRUCT_EQUALITY_OPERATORS_2(PathComponentKey, path, type)

  uint64_t hash() const
  {
    return get_default_hash(this->path, this->type);
  }
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

class JacobianSolver {
  Vector<Vector<float3>> position_offsets_;

 public:
  void offset_position(const int geo_i, const int point_i, const float3 &offset)
  {
    position_offsets_[geo_i][point_i] += offset;
  }
};

class GaussSeidelSolver {
  Span<SimPoints *> all_sim_points_;

 public:
  GaussSeidelSolver(Span<SimPoints *> all_sim_points) : all_sim_points_(all_sim_points) {}

  void offset_position(const int geo_i, const int point_i, const float3 &offset)
  {
    all_sim_points_[geo_i]->positions[point_i] += offset;
  }
};

class ConstraintSetEvaluator {
 public:
  virtual ~ConstraintSetEvaluator() = default;

  virtual void evaluate_jacobian(JacobianSolver &solver,
                                 const IndexMask &constraint_mask) const = 0;
  virtual void evaluate_gauss_seidel(GaussSeidelSolver &solver,
                                     const IndexMask &constraint_mask) const = 0;
};

template<typename Child> class TemplatedConstraintSetEvaluator : public ConstraintSetEvaluator {
  TemplatedConstraintSetEvaluator() = default;
  friend Child;

 public:
  void evaluate_jacobian(JacobianSolver &solver, const IndexMask &constraint_mask) const override
  {
    const Child &self = static_cast<const Child &>(*this);
    self.evaluate(solver, constraint_mask);
  }

  void evaluate_gauss_seidel(GaussSeidelSolver &solver,
                             const IndexMask &constraint_mask) const override
  {
    const Child &self = static_cast<const Child &>(*this);
    self.evaluate(solver, constraint_mask);
  }

  template<typename SolverT> void evaluate(SolverT &solver, const IndexMask &constraint_mask) const
  {
    constraint_mask.foreach_index(GrainSize(256), [&](const int i) {
      const Child &self = static_cast<const Child &>(*this);
      self.evaluate_single(solver, i);
    });
  }
};

class PinConstraintEvaluator : public TemplatedConstraintSetEvaluator<PinConstraintEvaluator> {
 private:
  int geo_i_;
  Span<float3> positions_;
  Span<int> indices_;
  Span<float3> pin_positions_;

 public:
  PinConstraintEvaluator(const int geo_i,
                         const Span<float3> positions,
                         const Span<int> indices,
                         const Span<float3> pin_positions)
      : geo_i_(geo_i), positions_(positions), indices_(indices), pin_positions_(pin_positions)
  {
  }

  template<typename SolverT> void evaluate_single(SolverT &solver, const int constraint_i) const
  {
    const int i = indices_[constraint_i];
    const float3 &pin_position = pin_positions_[constraint_i];
    const float3 &p = positions_[i];
    const float3 offset = pin_position - p;
    solver.offset_position(geo_i_, i, offset);
  }
};

class DistanceConstraintEvaluator
    : public TemplatedConstraintSetEvaluator<DistanceConstraintEvaluator> {
 private:
  int geo_i_;
  Span<float3> positions_;
  VArray<float> masses_;
  Span<int2> point_pairs_;
  Span<float> distances_;

 public:
  DistanceConstraintEvaluator(const int geo_i,
                              const Span<float3> positions,
                              const VArray<float> &masses,
                              const Span<int2> point_pairs,
                              const Span<float> distances)
      : geo_i_(geo_i),
        positions_(positions),
        masses_(masses),
        point_pairs_(point_pairs),
        distances_(distances)
  {
    BLI_assert(point_pairs.size() == distances.size());
  }

  template<typename SolverT> void evaluate_single(SolverT &solver, const int constraint_i) const
  {
    const int2 &point_pair = point_pairs_[constraint_i];
    const float target_distance = distances_[constraint_i];
    const int v0 = point_pair[0];
    const int v1 = point_pair[1];
    const float3 &p0 = positions_[v0];
    const float3 &p1 = positions_[v1];
    const float m0 = masses_[v0];
    const float m1 = masses_[v1];
    const float compliance_term = 0.0f;

    const float inv_m0 = 1.0f / m0;
    const float inv_m1 = 1.0f / m1;

    const float3 p_diff = p1 - p0;
    float length;
    const float3 normalized_dir = math::normalize_and_get_length(p_diff, length);
    const float length_diff = length - target_distance;
    const float lambda = length_diff / (inv_m0 + inv_m1 + compliance_term);

    const float3 offset0 = lambda * inv_m0 * normalized_dir;
    const float3 offset1 = -lambda * inv_m1 * normalized_dir;
    solver.offset_position(geo_i_, v0, offset0);
    solver.offset_position(geo_i_, v1, offset1);
  }
};

enum class ConstraintSetIndicesType {
  Unary,
  Binary,
};

class ConstraintSetIndices {
 public:
  virtual ~ConstraintSetIndices() = default;

  const ConstraintSetIndicesType type;
  int constraints_num;

  ConstraintSetIndices(const ConstraintSetIndicesType type, const int constraints_num)
      : type(type), constraints_num(constraints_num)
  {
  }
};

class UnaryConstraintSetIndices : public ConstraintSetIndices {
 public:
  int geo_i;
  Span<int> points;

  UnaryConstraintSetIndices(const int geo_i, const Span<int> points)
      : ConstraintSetIndices(ConstraintSetIndicesType::Unary, points.size()),
        geo_i(geo_i),
        points(points)
  {
  }
};

class BinaryConstraintSetIndices : public ConstraintSetIndices {
 public:
  int geo_i;
  Span<int2> point_pairs;

  BinaryConstraintSetIndices(const int geo_i, const Span<int2> point_pairs)
      : ConstraintSetIndices(ConstraintSetIndicesType::Binary, point_pairs.size()),
        geo_i(geo_i),
        point_pairs(point_pairs)
  {
  }
};

struct ConstraintSet {
  ConstraintSetIndices *indices;
  ConstraintSetEvaluator *evaluator;
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

static void solve_constraints(Span<SimPoints *> all_sim_points,
                              const Span<ConstraintSet> constraint_sets)
{
  GaussSeidelSolver solver{all_sim_points};
  for (const ConstraintSet &constraint_set : constraint_sets) {
    for (const int constraint_i : IndexRange(constraint_set.indices->constraints_num)) {
      constraint_set.evaluator->evaluate_gauss_seidel(solver,
                                                      IndexRange::from_single(constraint_i));
    }
  }
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

static void integrate_forces_and_accelerations(
    XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const Map<PathComponentKey, VArray<float>> &masses_map,
    const float delta_time)
{
  float3 gravity{0, 0, 0};
  for (const GravityBundle &gravity_bundle : world.gravities) {
    gravity = gravity_bundle.gravity;
  }

  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];
    PathSimData *path_sim_data = state.data_by_path.lookup_ptr(geometry_bundle.self_path);
    if (!path_sim_data) {
      continue;
    }
    Vector<const ForceBundle *> used_forces = filter_bundles_for_path<ForceBundle>(
        world.forces, geometry_bundle.self_path);
    for (const bke::GeometryComponent::Type type : {bke::GeometryComponent::Type::Mesh}) {
      const bke::GeometryComponent *component = applied_geometry.get_component(type);
      if (!component) {
        continue;
      }
      SimPoints *sim_points = path_sim_data->points_by_type.lookup_ptr(type);
      if (!sim_points) {
        continue;
      }
      const VArray<float> &masses = masses_map.lookup({geometry_bundle.self_path, type});
      Array<float3> force_sums(sim_points->points_num, float3(0.0f));
      bke::GeometryFieldContext field_context(*component, bke::AttrDomain::Point);
      for (const ForceBundle *force_bundle : used_forces) {
        fn::FieldEvaluator force_evaluator{field_context, sim_points->points_num};
        force_evaluator.set_selection(force_bundle->selection);
        force_evaluator.add(force_bundle->force);
        force_evaluator.evaluate();
        const IndexMask mask = force_evaluator.get_evaluated_selection_as_mask();
        const VArray<float3> force = force_evaluator.get_evaluated<float3>(0);
        mask.foreach_index(GrainSize(1024), [&](const int i) { force_sums[i] += force[i]; });
      }
      threading::parallel_for(
          IndexRange(sim_points->points_num), 1024, [&](const IndexRange range) {
            for (const int i : range) {
              const float mass = masses[i];
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

static void update_velocities(XPBDState &state)
{
  // TODO
}

/**
 * Computes the mass for each point. The mass of points that are known to be pinned have a mass of
 * infinity.
 */
static Map<PathComponentKey, VArray<float>> compute_masses(
    ResourceScope &scope, const WorldData &world, const Span<GeometrySet> applied_geometries)
{
  Map<PathComponentKey, VArray<float>> masses_map;
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
      auto &field_context = scope.construct<bke::GeometryFieldContext>(*component, domain);
      auto &mass_evaluator = scope.construct<fn::FieldEvaluator>(field_context, domain_size);
      mass_evaluator.add(geometry_bundle.mass);
      mass_evaluator.evaluate();
      VArray<float> masses = mass_evaluator.get_evaluated<float>(0);
      for (const PinnedPositionXPBDConstraintBundle *constraint : pinned_position_constraints) {
        fn::FieldEvaluator pin_evaluator{field_context, domain_size};
        pin_evaluator.set_selection(constraint->selection);
        pin_evaluator.evaluate();
        const IndexMask mask = pin_evaluator.get_evaluated_selection_as_mask();
        if (!mask.is_empty()) {
          Array<float> masses_array(domain_size);
          masses.materialize(masses_array);
          mask.foreach_index(GrainSize(1024), [&](const int i) {
            masses_array[i] = std::numeric_limits<float>::infinity();
          });
          masses = VArray<float>::from_container(std::move(masses_array));
        }
      }
      masses_map.add_new({geometry_bundle.self_path, type}, std::move(masses));
    }
  }
  return masses_map;
}

static void gather_distance_constraints(
    ResourceScope &scope,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const Map<std::string, Map<bke::GeometryComponent::Type, int>> &all_sim_points_keys,
    const Span<SimPoints *> all_sim_points,
    const Map<PathComponentKey, VArray<float>> &masses_map,
    Vector<ConstraintSet> &r_constraint_sets)
{
  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];
    if (!applied_geometry.has_mesh()) {
      continue;
    }
    const int geo_i = all_sim_points_keys.lookup(geometry_bundle.self_path)
                          .lookup(bke::GeometryComponent::Type::Mesh);
    const SimPoints &sim_points = *all_sim_points[geo_i];
    const Mesh &mesh = *applied_geometry.get_mesh();
    const Span<int2> mesh_edges = mesh.edges();
    const VArray<float> &masses = masses_map.lookup(
        {geometry_bundle.self_path, bke::GeometryComponent::Type::Mesh});

    const Vector<const EdgeLengthXPBDConstraintBundle *> edge_length_constraints =
        filter_bundles_for_path<EdgeLengthXPBDConstraintBundle>(world.edge_length_constraints,
                                                                geometry_bundle.self_path);
    bke::MeshFieldContext field_context(mesh, bke::AttrDomain::Edge);
    for (const EdgeLengthXPBDConstraintBundle *constraint_bundle : edge_length_constraints) {
      fn::FieldEvaluator field_evaluator{field_context, mesh_edges.size()};
      field_evaluator.set_selection(constraint_bundle->selection);
      field_evaluator.add(constraint_bundle->length);
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
      MutableSpan<float> constraint_lengths = scope.allocator().allocate_array<float>(mask.size());
      const VArray<float> length_varray = field_evaluator.get_evaluated<float>(0);
      length_varray.materialize_compressed(mask, constraint_lengths);
      r_constraint_sets.append(ConstraintSet{
          &scope.construct<BinaryConstraintSetIndices>(geo_i, constraint_edges),
          &scope.construct<DistanceConstraintEvaluator>(
              geo_i, sim_points.positions, masses, constraint_edges, constraint_lengths)});
    }
  }
}

static void gather_pin_constraints(
    ResourceScope &scope,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const Map<std::string, Map<bke::GeometryComponent::Type, int>> &all_sim_points_keys,
    const Span<SimPoints *> all_sim_points,
    Vector<ConstraintSet> &r_constraint_sets)
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

      const int geo_i = all_sim_points_keys.lookup(geometry_bundle.self_path).lookup(type);
      const SimPoints &sim_points = *all_sim_points[geo_i];

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
        r_constraint_sets.append(ConstraintSet{
            &scope.construct<UnaryConstraintSetIndices>(geo_i, constraint_indices),
            &scope.construct<PinConstraintEvaluator>(
                geo_i, sim_points.positions, constraint_indices, constraint_positions)});
      }
    }
  }
}

static void update_and_step_xpbd_state(XPBDState &state,
                                       const WorldData &world,
                                       const float delta_time,
                                       const int substeps)
{
  ResourceScope scope;
  Array<GeometrySet> applied_geometries(world.geometries.size());

  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    GeometrySet &applied_geometry = applied_geometries[bundle_i];
    applied_geometry = apply_simulation(geometry_bundle, state);
  }
  const Map<PathComponentKey, VArray<float>> masses_map = compute_masses(
      scope, world, applied_geometries);

  Map<std::string, PathSimData> new_data_by_path;
  for (const int bundle_i : world.geometries.index_range()) {
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];
    update_xpbd_state_for_geometry(
        state, world.geometries[bundle_i], applied_geometry, new_data_by_path);
  }
  state.data_by_path = std::move(new_data_by_path);

  Vector<SimPoints *> all_sim_points;
  Map<std::string, Map<bke::GeometryComponent::Type, int>> all_sim_points_keys;
  for (const int bundle_i : world.geometries.index_range()) {
    const XPBDGeometryBundle &geometry_bundle = world.geometries[bundle_i];
    PathSimData &path_sim_data = state.data_by_path.lookup(geometry_bundle.self_path);
    for (auto item : path_sim_data.points_by_type.items()) {
      const bke::GeometryComponent::Type type = item.key;
      SimPoints &sim_points = item.value;
      const int geo_i = all_sim_points.append_and_get_index(&sim_points);
      all_sim_points_keys.lookup_or_add_default_as(geometry_bundle.self_path).add_new(type, geo_i);
    }
  }

  Vector<ConstraintSet> constraint_sets;
  gather_distance_constraints(scope,
                              world,
                              applied_geometries,
                              all_sim_points_keys,
                              all_sim_points,
                              masses_map,
                              constraint_sets);
  gather_pin_constraints(
      scope, world, applied_geometries, all_sim_points_keys, all_sim_points, constraint_sets);

  for ([[maybe_unused]] const int substep_i : IndexRange(substeps)) {
    integrate_forces_and_accelerations(state, world, applied_geometries, masses_map, delta_time);
    solve_constraints(all_sim_points, constraint_sets);
    update_velocities(state);
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
