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
  types.append(SphericalSelfCollisionXPBDConstraintBundle::get_bundle_type());
  types.append(CurveSegmentXPBDConstraintBundle::get_bundle_type());
  types.append(OverpressureXPBDConstraintBundle::get_bundle_type());
  types.append(DampingBundle::get_bundle_type());
  types.append(TorqueBundle::get_bundle_type());
  types.append(PinnedRotationXPBDConstraintBundle::get_bundle_type());
  types.append(RodStretchAndShearXPBDConstraintBundle::get_bundle_type());
  types.append(RodBendAndTwistXPBDConstraintBundle::get_bundle_type());
  types.append(AlignPositionsConstraintBundle::get_bundle_type());

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
  Span<float3> inertias;
  Span<float3> inverse_inertias;
  float linear_damping;
  float angular_damping;
};

struct DistanceConstraintLengths {
  struct LengthItem {
    float length;
    bool used = true;
  };

  Map<OrderedEdge, LengthItem> lengths;
};

struct RelativeRotations {
  struct RotationItem {
    math::Quaternion rotation;
    bool used = true;
  };

  Map<int2, RotationItem> rotations;
};

struct PinPositionsState {
  struct Item {
    float3 position;
    bool used = true;
  };

  Map<int, Item> positions;
};

struct PinRotationsState {
  struct Item {
    math::Quaternion rotation;
    bool used = true;
  };

  Map<int, Item> rotations;
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
  Map<SimPointsKey, RelativeRotations> relative_rotations;
  Map<SimPointsKey, PinPositionsState> pin_positions;
  Map<SimPointsKey, PinRotationsState> pin_rotations;
  Map<SimPointsKey, float> initial_volumes;
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
  BundleVectorSet<CurveSegmentXPBDConstraintBundle> curve_segment_constraints;
  BundleVectorSet<PinnedPositionXPBDConstraintBundle> pinned_position_constraints;
  BundleVectorSet<InfiniteGroundPlaneBundle> infinite_ground_planes;
  BundleVectorSet<SphericalSelfCollisionXPBDConstraintBundle> spherical_self_collision_constraints;
  BundleVectorSet<OverpressureXPBDConstraintBundle> overpressure_constraints;
  BundleVectorSet<DampingBundle> dampings;
  BundleVectorSet<TorqueBundle> torques;
  BundleVectorSet<PinnedRotationXPBDConstraintBundle> pinned_rotation_constraints;
  BundleVectorSet<RodStretchAndShearXPBDConstraintBundle> rod_stretch_and_shear_constraints;
  BundleVectorSet<RodBendAndTwistXPBDConstraintBundle> rod_bend_and_twist_constraints;
  BundleVectorSet<AlignPositionsConstraintBundle> align_position_constraints;
};

template<typename T> struct StartStopPair {
  T start;
  T stop;

  T interpolate(const float factor) const
  {
    return math::interpolate(start, stop, factor);
  }
};

struct PinnedPositions {
  Vector<int> hard_indices;
  Vector<StartStopPair<float3>> hard_animations;

  Vector<int> soft_indices;
  Vector<float> soft_compliances;
  Vector<StartStopPair<float3>> soft_animations;
};

struct PinnedRotations {
  Vector<int> hard_indices;
  Vector<StartStopPair<math::Quaternion>> hard_animations;

  Vector<int> soft_indices;
  Vector<float> soft_compliances;
  Vector<StartStopPair<math::Quaternion>> soft_animations;
};

static AttrDomain get_simulation_domain(const bke::GeometryComponent::Type type)
{
  return type == bke::GeometryComponent::Type::Instance ? AttrDomain::Instance : AttrDomain::Point;
}

template<typename T>
static void parse_bundle(HandleNestedBundleParams &params,
                         BundleParseErrors errors,
                         WorldData::BundleVectorSet<T> &r_bundles)
{
  if (params.type != T::name) {
    return;
  }
  std::optional<T> parsed_bundle = T::parse(params.bundle, errors);
  if (!parsed_bundle) {
    return;
  }
  parsed_bundle->self_path = Bundle::combine_path(params.path);
  r_bundles.add_new(std::move(*parsed_bundle));
}

static WorldData parse_world(const Bundle &world_bundle)
{
  WorldData world;
  nested_bundle_foreach(world_bundle, [&](HandleNestedBundleParams &params) {
    BundleParseErrors errors;
    parse_bundle(params, errors, world.forces);
    parse_bundle(params, errors, world.gravities);
    parse_bundle(params, errors, world.geometries);
    parse_bundle(params, errors, world.edge_length_constraints);
    parse_bundle(params, errors, world.curve_segment_constraints);
    parse_bundle(params, errors, world.pinned_position_constraints);
    parse_bundle(params, errors, world.infinite_ground_planes);
    parse_bundle(params, errors, world.spherical_self_collision_constraints);
    parse_bundle(params, errors, world.overpressure_constraints);
    parse_bundle(params, errors, world.dampings);
    parse_bundle(params, errors, world.torques);
    parse_bundle(params, errors, world.pinned_rotation_constraints);
    parse_bundle(params, errors, world.rod_stretch_and_shear_constraints);
    parse_bundle(params, errors, world.rod_bend_and_twist_constraints);
    parse_bundle(params, errors, world.align_position_constraints);
  });
  return world;
}

static void integrate_linear_velocities(
    XPBDState &state,
    const Map<SimPointsKey, Span<float3>> &accelerations_map,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const float delta_time)
{
  for (auto item : state.sim_points.items()) {
    SimPoints &sim_points = item.value;
    const Span<float3> accelerations = accelerations_map.lookup(item.key);
    const SimPointsWorldProperties &props = sim_points_props.lookup(item.key);
    const int points_num = sim_points.positions.size();

    /* Approximation of exponential decay. */
    const float linear_damping_factor = std::max(1.0f - props.linear_damping * delta_time, 0.0f);

    threading::parallel_for(IndexRange(points_num), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        const float3 &acceleration = accelerations[i];
        sim_points.velocities[i] += acceleration * delta_time;
        sim_points.velocities[i] *= linear_damping_factor;
        sim_points.positions[i] += sim_points.velocities[i] * delta_time;
      }
    });
  }
}

static void integrate_angular_velocities(
    XPBDState &state,
    const Map<SimPointsKey, Span<float3>> &torques_map,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const float delta_time)
{
  for (auto item : state.sim_points.items()) {
    SimPoints &sim_points = item.value;
    if (!sim_points.has_rotation) {
      continue;
    }
    const std::optional<Span<float3>> torques = torques_map.lookup_try(item.key);

    const SimPointsWorldProperties &props = sim_points_props.lookup(item.key);
    const int points_num = sim_points.positions.size();

    /* Approximation of exponential decay. */
    const float angular_damping_factor = std::max(1.0f - props.angular_damping * delta_time, 0.0f);

    threading::parallel_for(IndexRange(points_num), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        const float3 &external_torque = torques.has_value() ? (*torques)[i] : float3(0.0f);
        const float3 &inertia = props.inertias[i];
        const float3 &inverse_inertia = props.inverse_inertias[i];
        if (math::is_zero(inverse_inertia)) {
          continue;
        }
        float3 &angular_velocity = sim_points.angular_velocities[i];
        const float3 precession = math::cross(angular_velocity, angular_velocity * inertia);
        angular_velocity += delta_time * (external_torque - precession) * inverse_inertia;
        angular_velocity *= angular_damping_factor;
        math::Quaternion &rotation = sim_points.rotations[i];
        const math::Quaternion direction = rotation * math::Quaternion(0, angular_velocity);
        rotation = math::normalize(
            math::Quaternion(float4(rotation) + delta_time * 0.5f * float4(direction)));
      }
    });
  }
}

static void store_rotation_if_necessary(const XPBDGeometryBundle &bundle,
                                        const SimPoints &sim_points,
                                        const bke::AttrDomain domain,
                                        bke::MutableAttributeAccessor attributes)
{
  if (!bundle.has_rotation || !sim_points.has_rotation || bundle.output_rotation_name.empty()) {
    return;
  }
  attributes.remove(bundle.output_rotation_name);
  bke::SpanAttributeWriter<math::Quaternion> attribute =
      attributes.lookup_or_add_for_write_only_span<math::Quaternion>(bundle.output_rotation_name,
                                                                     domain);
  attribute.span.copy_from(sim_points.rotations);
  attribute.finish();
}

static void apply_simulation_to_mesh(const XPBDGeometryBundle &bundle,
                                     GeometrySet &geometry,
                                     const SimPoints &sim_points)
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

  store_rotation_if_necessary(bundle, sim_points, AttrDomain::Point, mesh->attributes_for_write());
}

static void apply_simulation_to_pointcloud(const XPBDGeometryBundle &bundle,
                                           GeometrySet &geometry,
                                           const SimPoints &sim_points)
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

  store_rotation_if_necessary(
      bundle, sim_points, AttrDomain::Point, pointcloud->attributes_for_write());
}

static void apply_simulation_to_curves(const XPBDGeometryBundle &bundle,
                                       GeometrySet &geometry,
                                       const SimPoints &sim_points)
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

  store_rotation_if_necessary(
      bundle, sim_points, AttrDomain::Point, curves.attributes_for_write());
}

static void apply_simulation_to_instances(const XPBDGeometryBundle &bundle,
                                          GeometrySet &geometry,
                                          const SimPoints &sim_points)
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
  threading::parallel_for(transforms.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      transforms[i].location() = positions[i];
    }
  });

  store_rotation_if_necessary(
      bundle, sim_points, AttrDomain::Instance, instances->attributes_for_write());
}

static GeometrySet apply_simulation(const XPBDGeometryBundle &bundle, const XPBDState &state)
{
  GeometrySet geometry = bundle.geometry;

  if (geometry.has_mesh()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Mesh};
    if (const SimPoints *sim_points = state.sim_points.lookup_ptr(key)) {
      apply_simulation_to_mesh(bundle, geometry, *sim_points);
    }
  }
  if (geometry.has_pointcloud()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::PointCloud};
    if (const SimPoints *sim_points = state.sim_points.lookup_ptr(key)) {
      apply_simulation_to_pointcloud(bundle, geometry, *sim_points);
    }
  }
  if (geometry.has_curves()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Curve};
    if (const SimPoints *sim_points = state.sim_points.lookup_ptr(key)) {
      apply_simulation_to_curves(bundle, geometry, *sim_points);
    }
  }
  if (geometry.has_instances()) {
    const SimPointsKey key = {bundle.self_path, bke::GeometryComponent::Type::Instance};
    if (const SimPoints *sim_points = state.sim_points.lookup_ptr(key)) {
      apply_simulation_to_instances(bundle, geometry, *sim_points);
    }
  }

  return geometry;
}

static void ensure_rotation_data(SimPoints &sim_points,
                                 const XPBDGeometryBundle &bundle,
                                 const fn::FieldContext &field_context)
{
  if (bundle.has_rotation) {
    if (!sim_points.has_rotation) {
      /* Initialize new rotation data. */
      sim_points.has_rotation = true;
      sim_points.rotations.reinitialize(sim_points.points_num);
      sim_points.angular_velocities.reinitialize(sim_points.points_num);
      sim_points.angular_velocities.fill(float3(0.0f));
      fn::FieldEvaluator field_evaluator{field_context, sim_points.points_num};
      field_evaluator.add_with_destination(bundle.initial_rotations,
                                           sim_points.rotations.as_mutable_span());
      field_evaluator.evaluate();
      threading::parallel_for(
          IndexRange(sim_points.points_num), 1024, [&](const IndexRange range) {
            for (const int i : range) {
              sim_points.rotations[i] = math::normalize(sim_points.rotations[i]);
            }
          });
    }
  }
  else {
    if (sim_points.has_rotation) {
      /* Remove the simulation data because it was disabled on the bundle. */
      sim_points.has_rotation = false;
      sim_points.rotations.reinitialize(0);
      sim_points.angular_velocities.reinitialize(0);
    }
  }
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
    ensure_rotation_data(
        *new_sim_points, bundle, bke::MeshFieldContext(*current_mesh, AttrDomain::Point));
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
    ensure_rotation_data(
        *new_sim_points, bundle, bke::PointCloudFieldContext(*current_pointcloud));
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
    ensure_rotation_data(
        *new_sim_points, bundle, bke::CurvesFieldContext(*current_curves_id, AttrDomain::Point));
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
      sim_points.positions.reinitialize(instances_num);
      sim_points.velocities.reinitialize(instances_num);
      sim_points.velocities.fill(float3(0.0f));
      const Span<float4x4> transforms = current_instances->transforms();
      threading::parallel_for(
          sim_points.positions.index_range(), 1024, [&](const IndexRange range) {
            for (const int i : range) {
              sim_points.positions[i] = transforms[i].location();
            }
          });
      ensure_rotation_data(sim_points, bundle, bke::InstancesFieldContext(*current_instances));
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

static Vector<int> filter_sim_points_keys(const StringRef self_path,
                                          const StringRef filter,
                                          const Span<SimPointsKey> keys)
{
  Vector<int> filtered_keys;
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (nested_bundle_path_is_selected(self_path, filter, key.path)) {
      filtered_keys.append(key_i);
    }
  }
  return filtered_keys;
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
    const bke::AttrDomain domain = get_simulation_domain(type);
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

static Map<SimPointsKey, Span<float3>> compute_external_torques(
    ResourceScope &scope,
    const XPBDState &state,
    const WorldData &world,
    const Span<SimPointsKey> keys,
    const Span<GeometrySet> applied_geometries)
{
  Map<SimPointsKey, Span<float3>> torques_map;
  for (const SimPointsKey &sim_points_key : keys) {
    const int geometry_i = world.geometries.index_of_as(sim_points_key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_i];
    const SimPoints &sim_points = state.sim_points.lookup(sim_points_key);
    if (!sim_points.has_rotation) {
      continue;
    }
    const bke::GeometryComponent::Type type = sim_points_key.type;
    const bke::GeometryComponent *component = applied_geometry.get_component(type);
    if (!component) {
      continue;
    }
    Vector used_torques = filter_bundles_for_path<TorqueBundle>(world.torques,
                                                                sim_points_key.path);
    if (used_torques.is_empty()) {
      continue;
    }
    const AttrDomain domain = get_simulation_domain(type);
    const int domain_size = component->attribute_domain_size(domain);
    MutableSpan<float3> result = scope.allocator().allocate_array<float3>(domain_size);
    result.fill(float3(0.0f));

    bke::GeometryFieldContext field_context(*component, domain);
    for (const TorqueBundle *torque_bundle : used_torques) {
      fn::FieldEvaluator field_evaluator{field_context, domain_size};
      field_evaluator.set_selection(torque_bundle->selection);
      field_evaluator.add(torque_bundle->torque);
      field_evaluator.evaluate();
      const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
      const VArray<float3> torques_varray = field_evaluator.get_evaluated<float3>(0);
      mask.foreach_index([&](const int i) { result[i] += torques_varray[i]; });
    }

    torques_map.add(sim_points_key, result);
  }
  return torques_map;
}

static Map<SimPointsKey, SimPointsWorldProperties> compute_sim_point_world_properties(
    ResourceScope &scope,
    const WorldData &world,
    const Span<SimPointsKey> keys,
    const Span<GeometrySet> applied_geometries,
    const Map<SimPointsKey, PinnedPositions> &pinned_positions_map,
    const Map<SimPointsKey, PinnedRotations> &pinned_rotations_map)
{
  Map<SimPointsKey, SimPointsWorldProperties> properties_map;
  for (const SimPointsKey &key : keys) {
    const int geometry_i = world.geometries.index_of_as(key.path);
    const XPBDGeometryBundle &geometry_bundle = world.geometries[geometry_i];
    const GeometrySet &applied_geometry = applied_geometries[geometry_i];
    const PinnedPositions *pinned_positions = pinned_positions_map.lookup_ptr(key);
    const PinnedRotations *pinned_rotations = pinned_rotations_map.lookup_ptr(key);

    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometry.get_component(type);
    if (!component) {
      continue;
    }

    const bke::AttrDomain domain = get_simulation_domain(type);
    const int domain_size = component->attribute_domain_size(domain);

    MutableSpan<float> result_masses = scope.allocator().allocate_array<float>(domain_size);
    MutableSpan<float> result_frictions = scope.allocator().allocate_array<float>(domain_size);

    MutableSpan<float3> result_inertias;
    MutableSpan<float3> result_inverse_inertias;

    auto &field_context = scope.construct<bke::GeometryFieldContext>(*component, domain);
    auto &field_evaluator = scope.construct<fn::FieldEvaluator>(field_context, domain_size);
    field_evaluator.add_with_destination(geometry_bundle.mass, result_masses);
    field_evaluator.add_with_destination(geometry_bundle.friction, result_frictions);
    if (geometry_bundle.has_rotation) {
      result_inertias = scope.allocator().allocate_array<float3>(domain_size);
      result_inverse_inertias = scope.allocator().allocate_array<float3>(domain_size);
      field_evaluator.add_with_destination(geometry_bundle.inertia, result_inertias);
    }
    field_evaluator.evaluate();

    /* Invert masses and inertias and clamp frictions. */
    threading::parallel_for(IndexRange(domain_size), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        result_masses[i] = std::max(0.0f, math::safe_rcp(result_masses[i]));
        result_frictions[i] = std::max(0.0f, result_frictions[i]);
      }
      if (geometry_bundle.has_rotation) {
        for (const int i : range) {
          result_inverse_inertias[i] = math::safe_rcp(result_inertias[i]);
        }
      }
    });

    /* Set mass of pinned points to infinity (i.e. the inverse mass is 0). */
    if (pinned_positions) {
      for (const int i : pinned_positions->hard_indices) {
        result_masses[i] = 0.0f;
      }
    }

    /* Set inertia of pinned rotations to infinity (i.e. the inverse inertia is 0). */
    if (pinned_rotations) {
      for (const int i : pinned_rotations->hard_indices) {
        result_inertias[i] = float3(std::numeric_limits<float>::infinity());
        result_inverse_inertias[i] = float3(0.0f);
      }
    }

    const Vector damping_bundles = filter_bundles_for_path<DampingBundle>(world.dampings,
                                                                          key.path);
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    for (const DampingBundle *damping_bundle : damping_bundles) {
      linear_damping += damping_bundle->linear_damping;
      angular_damping += damping_bundle->angular_damping;
    }

    properties_map.add_new(key,
                           {result_masses,
                            result_frictions,
                            result_inertias,
                            result_inverse_inertias,
                            linear_damping,
                            angular_damping});
  }
  return properties_map;
}

static Span<float> prepare_distance_constraint_lengths(ResourceScope &scope,
                                                       XPBDState &state,
                                                       const Span<float3> positions,
                                                       const SimPointsKey &key,
                                                       const Span<int2> segments)
{
  MutableSpan<float> constraint_lengths = scope.allocator().allocate_array<float>(segments.size());
  DistanceConstraintLengths &distance_constraint_lengths =
      state.distance_constraint_lengths.lookup_or_add_default(key);
  for (const int i : segments.index_range()) {
    const int2 &segment = segments[i];
    const OrderedEdge ordered_edge{segment[0], segment[1]};
    DistanceConstraintLengths::LengthItem &length_item =
        distance_constraint_lengths.lengths.lookup_or_add_cb(ordered_edge, [&]() {
          const float3 &p0 = positions[segment[0]];
          const float3 &p1 = positions[segment[1]];
          return DistanceConstraintLengths::LengthItem{math::distance(p0, p1)};
        });
    length_item.used = true;
    constraint_lengths[i] = length_item.length;
  }
  return constraint_lengths;
}

static Span<math::Quaternion> prepare_relative_rotations(ResourceScope &scope,
                                                         XPBDState &state,
                                                         const Span<math::Quaternion> rotations,
                                                         const SimPointsKey &key,
                                                         const Span<int2> pairs)
{
  MutableSpan<math::Quaternion> results = scope.allocator().allocate_array<math::Quaternion>(
      pairs.size());
  RelativeRotations &relative_rotations = state.relative_rotations.lookup_or_add_default(key);
  for (const int i : pairs.index_range()) {
    const int2 &pair = pairs[i];
    RelativeRotations::RotationItem &rotation_item = relative_rotations.rotations.lookup_or_add_cb(
        pair, [&]() {
          const math::Quaternion &r0 = rotations[pair[0]];
          const math::Quaternion &r1 = rotations[pair[1]];
          const math::Quaternion rotation = math::invert_normalized(r0) * r1;
          return RelativeRotations::RotationItem{rotation};
        });
    rotation_item.used = true;
    results[i] = rotation_item.rotation;
  }
  return results;
}

static float compute_compliance_factor(const float delta_time)
{
  return math::safe_rcp(pow2f(delta_time));
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
      const float compliance_factor = compute_compliance_factor(delta_time);
      threading::parallel_for(mask.index_range(), 512, [&](const IndexRange range) {
        for (float &compliance_term : compliance_terms.slice(range)) {
          compliance_term *= compliance_factor;
        }
      });

      /* Prepare per-constraint length. */
      const Span<float> constraint_lengths = prepare_distance_constraint_lengths(
          scope, state, mesh_positions, key, constraint_edges);

      /* Add the actual constraint. */
      r_constraint_sets.append(
          {scope.construct<geometry::xpbd_constraint_solver::BinaryConstraintSetIndices>(
               key_i, constraint_edges),
           scope.construct<geometry::xpbd_constraint_solver::DistanceConstraintEvaluator>(
               key_i, inverse_masses, constraint_edges, constraint_lengths, compliance_terms)});
    }
  }
}

static void gather_curve_segment_constraints(
    ResourceScope &scope,
    XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const float delta_time,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  const float compliance_factor = compute_compliance_factor(delta_time);

  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Curve) {
      continue;
    }
    const int geometry_bundle_i = world.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];

    const Curves &curves_id = *applied_geometry.get_curves();
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    const Span<float3> positions = curves.positions();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const VArray<bool> cyclics = curves.cyclic();
    const Span<float> inverse_masses = sim_points_props.lookup(key).inverse_masses;

    const Vector constraint_bundles = filter_bundles_for_path<CurveSegmentXPBDConstraintBundle>(
        world.curve_segment_constraints, key.path);

    bke::CurvesFieldContext field_context(curves, bke::AttrDomain::Point);
    for (const CurveSegmentXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      fn::FieldEvaluator field_evaluator{field_context, curves.points_num()};
      field_evaluator.add(constraint_bundle->compliance);
      field_evaluator.evaluate();
      const VArray<float> compliances = field_evaluator.get_evaluated<float>(0);

      Vector<int2> &constraint_segments = scope.construct<Vector<int2>>();
      Vector<float> &constraint_compliance_terms = scope.construct<Vector<float>>();
      for (const int curve_i : curves.curves_range()) {
        const IndexRange points = points_by_curve[curve_i];
        if (points.size() <= 1) {
          continue;
        }
        for (const int i : points.index_range().drop_back(1)) {
          const int point_i = points[i];
          const int next_point_i = point_i + 1;
          const float compliance = compliances[point_i];
          constraint_segments.append({point_i, next_point_i});
          constraint_compliance_terms.append(compliance * compliance_factor);
        }
        const bool cyclic = cyclics[curve_i];
        if (cyclic) {
          const int point_i = points.last();
          const int next_point_i = points.first();
          const float compliance = compliances[point_i];
          constraint_segments.append({point_i, next_point_i});
          constraint_compliance_terms.append(compliance * compliance_factor);
        }
      }

      const Span<float> constraint_lengths = prepare_distance_constraint_lengths(
          scope, state, positions, key, constraint_segments);

      r_constraint_sets.append(
          {scope.construct<geometry::xpbd_constraint_solver::BinaryConstraintSetIndices>(
               key_i, constraint_segments),
           scope.construct<geometry::xpbd_constraint_solver::DistanceConstraintEvaluator>(
               key_i,
               inverse_masses,
               constraint_segments,
               constraint_lengths,
               constraint_compliance_terms)});
    }
  }
}

static void gather_overpressure_constraints(
    ResourceScope &scope,
    XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Mesh) {
      continue;
    }
    const int geometry_bundle_i = world.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const Mesh &mesh = *applied_geometry.get_mesh();
    const Span<int3> tris = mesh.corner_tris();
    const Span<int> corner_verts = mesh.corner_verts();
    const Span<float3> positions = mesh.vert_positions();

    const Vector overpressure_constraints =
        filter_bundles_for_path<OverpressureXPBDConstraintBundle>(world.overpressure_constraints,
                                                                  key.path);
    const Span<float> inverse_masses = sim_points_props.lookup(key).inverse_masses;

    for (const OverpressureXPBDConstraintBundle *constraint_bundle : overpressure_constraints) {
      const float overpressure = constraint_bundle->overpressure;
      const float initial_volume = state.initial_volumes.lookup_or_add_cb(key, [&]() {
        return geometry::xpbd_constraint_solver::OverpressureConstraintEvaluator::compute_volume(
            tris, corner_verts, positions);
      });
      Vector<Vector<int>> &affected_points = scope.construct<Vector<Vector<int>>>();
      affected_points.append({});
      for (const int i : positions.index_range()) {
        affected_points[0].append(i);
      }
      r_constraint_sets.append(
          {scope.construct<geometry::xpbd_constraint_solver::NAryConstraintSetIndices>(
               key_i, affected_points),
           scope.construct<geometry::xpbd_constraint_solver::OverpressureConstraintEvaluator>(
               key_i, tris, corner_verts, inverse_masses, overpressure, initial_volume)});
    }
  }
}

static void gather_curves_rod_stretch_and_shear_constraints(
    ResourceScope &scope,
    XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const float delta_time,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  const float compliance_factor = compute_compliance_factor(delta_time);

  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Curve) {
      continue;
    }
    const SimPoints &sim_points = state.sim_points.lookup(key);
    if (!sim_points.has_rotation) {
      continue;
    }
    const int geometry_bundle_i = world.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const Curves &curves_id = *applied_geometry.get_curves();
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const SimPointsWorldProperties &props = sim_points_props.lookup(key);

    const Vector constraint_bundles =
        filter_bundles_for_path<RodStretchAndShearXPBDConstraintBundle>(
            world.rod_stretch_and_shear_constraints, key.path);

    bke::CurvesFieldContext field_context(curves, bke::AttrDomain::Point);
    for (const RodStretchAndShearXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      fn::FieldEvaluator field_evaluator{field_context, curves.points_num()};
      field_evaluator.add(constraint_bundle->compliance);
      field_evaluator.evaluate();
      const VArray<float> compliances = field_evaluator.get_evaluated<float>(0);

      Vector<int2> &constraint_segments = scope.construct<Vector<int2>>();
      Vector<float> &constraint_compliance_terms = scope.construct<Vector<float>>();

      for (const int curve_i : curves.curves_range()) {
        const IndexRange points = points_by_curve[curve_i];
        if (points.size() <= 1) {
          continue;
        }
        for (const int i : points.index_range().drop_back(1)) {
          const int point_i = points[i];
          const int next_point_i = point_i + 1;
          const float compliance = compliances[point_i];
          constraint_segments.append({point_i, next_point_i});
          constraint_compliance_terms.append(compliance * compliance_factor);
        }
        /* TODO: Handle cyclic curves. */
      }
      const Span<float> constraint_lenghts = prepare_distance_constraint_lengths(
          scope, state, sim_points.positions, key, constraint_segments);
      r_constraint_sets.append(
          {scope.construct<geometry::xpbd_constraint_solver::BinaryConstraintSetIndices>(
               key_i, constraint_segments),
           scope
               .construct<geometry::xpbd_constraint_solver::RodStretchAndShearConstraintEvaluator>(
                   key_i,
                   constraint_segments,
                   props.inverse_masses,
                   props.inertias,
                   constraint_lenghts,
                   constraint_compliance_terms)});
    }
  }
}

static void gather_curves_rod_bend_and_twist_constraints(
    ResourceScope &scope,
    XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const float delta_time,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  const float compliance_factor = compute_compliance_factor(delta_time);

  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Curve) {
      continue;
    }
    const SimPoints &sim_points = state.sim_points.lookup(key);
    if (!sim_points.has_rotation) {
      continue;
    }
    const int geometry_bundle_i = world.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const Curves &curves_id = *applied_geometry.get_curves();
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const SimPointsWorldProperties &props = sim_points_props.lookup(key);

    const Vector constraint_bundles = filter_bundles_for_path<RodBendAndTwistXPBDConstraintBundle>(
        world.rod_bend_and_twist_constraints, key.path);

    bke::CurvesFieldContext field_context(curves, bke::AttrDomain::Point);
    for (const RodBendAndTwistXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      fn::FieldEvaluator field_evaluator{field_context, curves.points_num()};
      field_evaluator.add(constraint_bundle->compliance);
      field_evaluator.evaluate();
      const VArray<float> compliances = field_evaluator.get_evaluated<float>(0);

      Vector<int2> &constraint_segments = scope.construct<Vector<int2>>();
      Vector<float> &constraint_compliance_terms = scope.construct<Vector<float>>();

      for (const int curve_i : curves.curves_range()) {
        const IndexRange points = points_by_curve[curve_i];
        if (points.size() <= 1) {
          continue;
        }
        for (const int i : points.index_range().drop_back(1)) {
          const int point_i = points[i];
          const int next_point_i = point_i + 1;
          const float compliance = compliances[point_i];
          constraint_segments.append({point_i, next_point_i});
          constraint_compliance_terms.append(compliance * compliance_factor);
        }
        /* TODO: Handle cyclic curves. */
      }
      const Span<math::Quaternion> rest_rotations = prepare_relative_rotations(
          scope, state, sim_points.rotations, key, constraint_segments);
      r_constraint_sets.append(
          {scope.construct<geometry::xpbd_constraint_solver::BinaryConstraintSetIndices>(
               key_i, constraint_segments),
           scope.construct<geometry::xpbd_constraint_solver::RodBendAndTwistConstraintEvaluator>(
               key_i,
               constraint_segments,
               props.inertias,
               rest_rotations,
               constraint_compliance_terms)});
    }
  }
}

static Map<SimPointsKey, MutableSpan<float3>> gather_soft_pinned_position_constraints(
    ResourceScope &scope,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const Map<SimPointsKey, PinnedPositions> &pinned_positions_map,
    const float delta_time,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  const float compliance_factor = compute_compliance_factor(delta_time);
  Map<SimPointsKey, MutableSpan<float3>> result;
  for (const auto item : pinned_positions_map.items()) {
    const SimPointsKey &key = item.key;
    const int key_i = keys.index_of(key);
    const PinnedPositions &pinned_positions = item.value;
    const Span<float> inverse_masses = sim_points_props.lookup(key).inverse_masses;
    const int constraints_num = pinned_positions.soft_indices.size();
    if (constraints_num == 0) {
      continue;
    }
    MutableSpan<float> compliance_terms = scope.allocator().allocate_array<float>(constraints_num);
    for (const int i : pinned_positions.soft_indices.index_range()) {
      compliance_terms[i] = pinned_positions.soft_compliances[i] * compliance_factor;
    }
    /* Positions are initialized in #update_pinned_positions. */
    MutableSpan<float3> soft_pinned_positions = scope.allocator().allocate_array<float3>(
        constraints_num);
    result.add(key, soft_pinned_positions);

    r_constraint_sets.append(
        {scope.construct<geometry::xpbd_constraint_solver::UnaryConstraintSetIndices>(
             key_i, pinned_positions.soft_indices),
         scope.construct<geometry::xpbd_constraint_solver::PinnedPositionConstraintEvaluator>(
             key_i,
             pinned_positions.soft_indices,
             soft_pinned_positions,
             compliance_terms,
             inverse_masses)});
  }
  return result;
}

static Map<SimPointsKey, MutableSpan<math::Quaternion>> gather_soft_pinned_rotation_constraints(
    ResourceScope &scope,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const Map<SimPointsKey, PinnedRotations> &pinned_rotations_map,
    const float delta_time,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  const float compliance_factor = compute_compliance_factor(delta_time);
  Map<SimPointsKey, MutableSpan<math::Quaternion>> result;
  for (const auto item : pinned_rotations_map.items()) {
    const SimPointsKey &key = item.key;
    const int key_i = keys.index_of(key);
    const PinnedRotations &pinned_rotations = item.value;
    const int constraints_num = pinned_rotations.soft_indices.size();
    if (constraints_num == 0) {
      continue;
    }

    MutableSpan<float> compliance_terms = scope.allocator().allocate_array<float>(constraints_num);
    for (const int i : pinned_rotations.soft_indices.index_range()) {
      compliance_terms[i] = pinned_rotations.soft_compliances[i] * compliance_factor;
    }

    /* Rotations are initialized in #update_pinned_rotations. */
    MutableSpan<math::Quaternion> soft_pinned_rotations =
        scope.allocator().allocate_array<math::Quaternion>(constraints_num);
    result.add(key, soft_pinned_rotations);

    r_constraint_sets.append(
        {scope.construct<geometry::xpbd_constraint_solver::UnaryConstraintSetIndices>(
             key_i, pinned_rotations.soft_indices),
         scope.construct<geometry::xpbd_constraint_solver::PinRotationConstraintEvaluator>(
             key_i,
             pinned_rotations.soft_indices,
             soft_pinned_rotations,
             compliance_terms,
             sim_points_props.lookup(key).inverse_inertias)});
  }
  return result;
}

static void gather_align_positions_constraints(
    ResourceScope &scope,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const float delta_time,
    Vector<geometry::xpbd_constraint_solver::ConstraintSet> &r_constraint_sets)
{
  const float compliance_factor = compute_compliance_factor(delta_time);

  for (const AlignPositionsConstraintBundle &constraint_bundle : world.align_position_constraints)
  {
    Vector<int> filtered_keys = filter_sim_points_keys(
        constraint_bundle.self_path, constraint_bundle.filter, keys);
    if (filtered_keys.is_empty()) {
      continue;
    }
    if (filtered_keys.size() >= 2) {
      /* TODO: Support aligning positions across geometries. */
      continue;
    }
    const int key_i = filtered_keys[0];
    const SimPointsKey &key = keys[key_i];
    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometries[key_i].get_component(type);
    if (!component) {
      continue;
    }
    const bke::AttrDomain domain = get_simulation_domain(type);
    const int domain_size = component->attribute_domain_size(domain);
    const Span<float> inverse_masses = sim_points_props.lookup(key).inverse_masses;

    const bke::GeometryFieldContext field_context(*component, domain);
    fn::FieldEvaluator field_evaluator(field_context, domain_size);
    field_evaluator.set_selection(constraint_bundle.selection);
    field_evaluator.add(constraint_bundle.group_id);
    field_evaluator.add(constraint_bundle.compliance);
    field_evaluator.evaluate();
    const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
    const VArray<int> group_ids = field_evaluator.get_evaluated<int>(0);
    const VArray<float> compliances = field_evaluator.get_evaluated<float>(1);

    MultiValueMap<int, int> points_by_group_id;
    mask.foreach_index([&](const int point_i) {
      const int group_id = group_ids[point_i];
      points_by_group_id.add(group_id, point_i);
    });

    Vector<int2> &constraint_pairs = scope.construct<Vector<int2>>();
    Vector<float> &constraint_compliance_terms = scope.construct<Vector<float>>();
    Vector<float> &constraint_distances = scope.construct<Vector<float>>();

    for (const Span<int> point_indices : points_by_group_id.values()) {
      if (point_indices.size() <= 1) {
        /* The constraint needs at least two points. */
        continue;
      }
      /* Compliance of the constraint is the geometric mean of the compliances of each point. */
      float compliances_log_sum = 0.0f;
      for (const int point_i : point_indices) {
        const float point_compliance = std::max(0.0f, compliances[point_i]);
        if (point_compliance > 0.0f) {
          compliances_log_sum += logf(point_compliance);
        }
      }
      const float compliances_log_mean = compliances_log_sum / point_indices.size();
      const float compliance = expf(compliances_log_mean);
      const float compliance_term = compliance * compliance_factor;
      /* TODO: Create a single constraint for all aligned points instead of pairing them.*/
      for (const int i : point_indices.index_range()) {
        for (const int j : point_indices.index_range().drop_front(i + 1)) {
          constraint_pairs.append({point_indices[i], point_indices[j]});
          constraint_compliance_terms.append(compliance_term);
          constraint_distances.append(0.0f);
        }
      }
    }

    r_constraint_sets.append(
        {scope.construct<geometry::xpbd_constraint_solver::BinaryConstraintSetIndices>(
             key_i, constraint_pairs),
         scope.construct<geometry::xpbd_constraint_solver::DistanceConstraintEvaluator>(
             key_i,
             inverse_masses,
             constraint_pairs,
             constraint_distances,
             constraint_compliance_terms)});
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
                                         const Span<float> sim_points_inverse_masses,
                                         StaticPlaneContacts &r_contacts)
{
  const float3 plane_normal = math::normalize(ground_plane.normal);
  if (math::is_zero(plane_normal)) {
    return;
  }
  for (const int point_i : IndexRange(sim_points.points_num)) {
    const float inverse_mass = sim_points_inverse_masses[point_i];
    if (math::is_zero(inverse_mass)) {
      /* Points with infinite mass are pinned and don't collide dynamically. */
      continue;
    }

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
    const SimPointsWorldProperties &props = sim_points_props.lookup(key);

    {
      const Vector ground_plane_bundles = filter_bundles_for_path<InfiniteGroundPlaneBundle>(
          world.infinite_ground_planes, key.path);
      StaticPlaneContacts plane_contacts;
      for (const InfiniteGroundPlaneBundle *ground_plane_bundle : ground_plane_bundles) {
        gather_ground_plane_contacts(sim_points,
                                     *ground_plane_bundle,
                                     props.frictions,
                                     props.inverse_masses,
                                     plane_contacts);
      }
      if (!plane_contacts.indices.is_empty()) {
        contacts.static_plane_contacts.add_new(key, std::move(plane_contacts));
      }
    }
    const Vector spherical_self_collision_constraints =
        filter_bundles_for_path<SphericalSelfCollisionXPBDConstraintBundle>(
            world.spherical_self_collision_constraints, key.path);
    std::optional<VArray<float>> radii;
    if (type == bke::GeometryComponent::Type::PointCloud) {
      const bke::PointCloudComponent &pointcloud_component =
          *static_cast<const bke::PointCloudComponent *>(component);
      if (const PointCloud *pointcloud = pointcloud_component.get()) {
        radii.emplace(pointcloud->radius());
      }
    }
    else if (type == bke::GeometryComponent::Type::Curve) {
      const bke::CurveComponent &curves_component = *static_cast<const bke::CurveComponent *>(
          component);
      if (const Curves *curves_id = curves_component.get()) {
        const bke::CurvesGeometry &curves = curves_id->geometry.wrap();
        radii.emplace(curves.radius());
      }
    }
    if (radii.has_value()) {
      const VArraySpan<float> radii_span = *radii;
      DynamicSphereContacts sphere_contacts;
      for ([[maybe_unused]] const SphericalSelfCollisionXPBDConstraintBundle *constraint_bundle :
           spherical_self_collision_constraints)
      {
        /* TODO: Avoid self collisions with direct neighbor.*/
        gather_sphere_contacts(sim_points, radii_span, sphere_contacts);
      }
      if (!sphere_contacts.indices.is_empty()) {
        contacts.dynamic_sphere_contacts.add_new(key, std::move(sphere_contacts));
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
             key_i,
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
             key_i,
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

static void reset_state_usages(XPBDState &state)
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
  for (PinPositionsState &pin_positions : state.pin_positions.values()) {
    for (PinPositionsState::Item &item : pin_positions.positions.values()) {
      item.used = false;
    }
  }
  for (PinRotationsState &pin_rotations : state.pin_rotations.values()) {
    for (PinRotationsState::Item &item : pin_rotations.rotations.values()) {
      item.used = false;
    }
  }
}

static void remove_unused_states(XPBDState &state)
{
  for (DistanceConstraintLengths &distance_constraint_lengths :
       state.distance_constraint_lengths.values())
  {
    distance_constraint_lengths.lengths.remove_if(
        [](const auto &item) { return !item.value.used; });
  }
  for (PinPositionsState &pin_positions : state.pin_positions.values()) {
    pin_positions.positions.remove_if([](const auto &item) { return !item.value.used; });
  }
  for (PinRotationsState &pin_rotations : state.pin_rotations.values()) {
    pin_rotations.rotations.remove_if([](const auto &item) { return !item.value.used; });
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

static void update_linear_velocities(XPBDState &state,
                                     const VectorSet<SimPointsKey> &keys,
                                     const Span<Array<float3>> all_prev_positions,
                                     const float delta_time)
{
  const float inv_delta_time = math::safe_rcp(delta_time);
  for (const int key_i : keys.index_range()) {
    const Span<float3> prev_positions = all_prev_positions[key_i];
    SimPoints &sim_points = state.sim_points.lookup(keys[key_i]);
    Span<float3> new_positions = sim_points.positions;
    MutableSpan<float3> velocities = sim_points.velocities;
    threading::parallel_for(IndexRange(sim_points.points_num), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        const float3 &prev_position = prev_positions[i];
        const float3 &new_position = new_positions[i];
        const float3 diff = new_position - prev_position;
        const float3 velocity = diff * inv_delta_time;
        velocities[i] = velocity;
      }
    });
  }
}

static void update_angular_velocities(XPBDState &state,
                                      const VectorSet<SimPointsKey> &keys,
                                      const Span<Array<math::Quaternion>> all_prev_rotations,
                                      const float delta_time)
{
  const float inv_delta_time = math::safe_rcp(delta_time);
  for (const int key_i : keys.index_range()) {
    SimPoints &sim_points = state.sim_points.lookup(keys[key_i]);
    if (!sim_points.has_rotation) {
      continue;
    }
    const Span<math::Quaternion> prev_rotations = all_prev_rotations[key_i];
    const Span<math::Quaternion> new_rotations = sim_points.rotations;
    MutableSpan<float3> angular_velocities = sim_points.angular_velocities;
    threading::parallel_for(IndexRange(sim_points.points_num), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        float3 diff =
            (math::invert_normalized(prev_rotations[i]) * new_rotations[i]).imaginary_part();
        for (const int j : IndexRange(3)) {
          if (math::abs(diff[j]) < 1e-5f) {
            diff[j] = 0.0f;
          }
        }

        const float3 new_angular_velocity = 2.0f * diff * inv_delta_time;
        angular_velocities[i] = new_angular_velocity;
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

static Map<SimPointsKey, PinnedPositions> compute_pinned_positions(
    XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys)
{
  Map<SimPointsKey, PinnedPositions> result;
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];

    const Vector constraint_bundle = filter_bundles_for_path<PinnedPositionXPBDConstraintBundle>(
        world.pinned_position_constraints, key.path);
    if (constraint_bundle.is_empty()) {
      continue;
    }
    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometry.get_component(type);
    if (!component) {
      continue;
    }
    const AttrDomain domain = get_simulation_domain(type);
    const int domain_size = component->attribute_domain_size(domain);

    PinPositionsState &pinned_positions_state = state.pin_positions.lookup_or_add_default(key);
    PinnedPositions pinned_positions;

    bke::GeometryFieldContext field_context(*component, domain);
    for (const PinnedPositionXPBDConstraintBundle *constraint_bundle : constraint_bundle) {
      fn::FieldEvaluator field_evaluator{field_context, domain_size};
      field_evaluator.set_selection(constraint_bundle->selection);
      field_evaluator.add(constraint_bundle->position);
      field_evaluator.add(constraint_bundle->compliance);
      field_evaluator.evaluate();
      const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
      if (mask.is_empty()) {
        continue;
      }
      const VArray<float3> pinned_positions_varray = field_evaluator.get_evaluated<float3>(0);
      const VArray<float> compliances_varray = field_evaluator.get_evaluated<float>(1);
      mask.foreach_index([&](const int point_i) {
        PinPositionsState::Item &old_position_item =
            pinned_positions_state.positions.lookup_or_add(point_i,
                                                           {pinned_positions_varray[point_i]});
        old_position_item.used = true;
        const float3 &new_position = pinned_positions_varray[point_i];
        const float compliance = std::max(0.0f, compliances_varray[point_i]);
        StartStopPair<float3> animation{old_position_item.position, new_position};
        if (compliance == 0.0f) {
          pinned_positions.hard_indices.append(point_i);
          pinned_positions.hard_animations.append(animation);
        }
        else {
          pinned_positions.soft_indices.append(point_i);
          pinned_positions.soft_compliances.append(compliance);
          pinned_positions.soft_animations.append(animation);
        }
        old_position_item.position = new_position;
      });
    }
    if (!pinned_positions.hard_indices.is_empty() || !pinned_positions.soft_indices.is_empty()) {
      result.add(key, std::move(pinned_positions));
    }
  }
  return result;
}

static Map<SimPointsKey, PinnedRotations> computed_pinned_rotation_animations(
    XPBDState &state,
    const WorldData &world,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys)
{
  Map<SimPointsKey, PinnedRotations> result;
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const SimPoints &sim_points = state.sim_points.lookup(key);
    if (!sim_points.has_rotation) {
      continue;
    }
    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometry.get_component(type);
    if (!component) {
      continue;
    }
    const AttrDomain domain = get_simulation_domain(type);
    const int domain_size = component->attribute_domain_size(domain);

    const Vector constraint_bundles = filter_bundles_for_path<PinnedRotationXPBDConstraintBundle>(
        world.pinned_rotation_constraints, key.path);

    PinRotationsState &pinned_rotations_state = state.pin_rotations.lookup_or_add_default(key);
    PinnedRotations pinned_rotations;

    bke::GeometryFieldContext field_context(*component, domain);
    for (const PinnedRotationXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      fn::FieldEvaluator field_evaluator{field_context, domain_size};
      field_evaluator.set_selection(constraint_bundle->selection);
      field_evaluator.add(constraint_bundle->rotation);
      field_evaluator.add(constraint_bundle->compliance);
      field_evaluator.evaluate();
      const IndexMask mask = field_evaluator.get_evaluated_selection_as_mask();
      if (mask.is_empty()) {
        continue;
      }
      const VArray<math::Quaternion> rotations_varray =
          field_evaluator.get_evaluated<math::Quaternion>(0);
      const VArray<float> compliances_varray = field_evaluator.get_evaluated<float>(1);
      mask.foreach_index([&](const int point_i) {
        PinRotationsState::Item &old_rotation_item =
            pinned_rotations_state.rotations.lookup_or_add(point_i, {rotations_varray[point_i]});
        old_rotation_item.used = true;
        const math::Quaternion &new_rotation = rotations_varray[point_i];
        const float compliance = std::max(0.0f, compliances_varray[point_i]);
        StartStopPair<math::Quaternion> animation{old_rotation_item.rotation, new_rotation};
        if (compliance == 0.0f) {
          pinned_rotations.hard_indices.append(point_i);
          pinned_rotations.hard_animations.append(animation);
        }
        else {
          pinned_rotations.soft_indices.append(point_i);
          pinned_rotations.soft_compliances.append(compliance);
          pinned_rotations.soft_animations.append(animation);
        }
        old_rotation_item.rotation = new_rotation;
      });
    }
    if (!pinned_rotations.hard_indices.is_empty() || !pinned_rotations.soft_indices.is_empty()) {
      result.add(key, std::move(pinned_rotations));
    }
  }
  return result;
}

static void update_pinned_positions(
    XPBDState &state,
    const Map<SimPointsKey, PinnedPositions> &pinned_positions_map,
    const Map<SimPointsKey, MutableSpan<float3>> &soft_pinned_positions_map,
    const float factor)
{
  for (const auto item : pinned_positions_map.items()) {
    const SimPointsKey &key = item.key;
    SimPoints &sim_points = state.sim_points.lookup(key);
    const PinnedPositions &pinned_positions = item.value;
    for (const int i : pinned_positions.hard_indices.index_range()) {
      const int point_i = pinned_positions.hard_indices[i];
      const float3 current_position = pinned_positions.hard_animations[i].interpolate(factor);
      sim_points.positions[point_i] = current_position;
    }
    if (!pinned_positions.soft_indices.is_empty()) {
      /* These positions are referenced by the constraints, so they get the new position. */
      MutableSpan<float3> soft_pinned_positions = soft_pinned_positions_map.lookup(key);
      for (const int i : pinned_positions.soft_indices.index_range()) {
        const float3 current_position = pinned_positions.soft_animations[i].interpolate(factor);
        soft_pinned_positions[i] = current_position;
      }
    }
  }
}

static void update_pinned_rotations(
    XPBDState &state,
    const Map<SimPointsKey, PinnedRotations> &pinned_rotations_map,
    const Map<SimPointsKey, MutableSpan<math::Quaternion>> &soft_pinned_rotations_map,
    const float factor)
{
  for (const auto item : pinned_rotations_map.items()) {
    const SimPointsKey &key = item.key;
    SimPoints &sim_points = state.sim_points.lookup(key);
    const PinnedRotations &pinned_rotations = item.value;
    for (const int i : pinned_rotations.hard_indices.index_range()) {
      const int point_i = pinned_rotations.hard_indices[i];
      const math::Quaternion current_rotation = pinned_rotations.hard_animations[i].interpolate(
          factor);
      sim_points.rotations[point_i] = current_rotation;
    }
    if (!pinned_rotations.soft_indices.is_empty()) {
      /* These rotations are referenced by the constraints, so they get the new rotation. */
      MutableSpan<math::Quaternion> soft_pinned_rotations = soft_pinned_rotations_map.lookup(key);
      for (const int i : pinned_rotations.soft_indices.index_range()) {
        const math::Quaternion current_rotation = pinned_rotations.soft_animations[i].interpolate(
            factor);
        soft_pinned_rotations[i] = current_rotation;
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
  const Array<GeometrySet> applied_geometries = gather_applied_geometries(state, world);
  update_sim_points_from_world(state, world, applied_geometries);

  VectorSet<SimPointsKey> keys;
  for (const SimPointsKey &key : state.sim_points.keys()) {
    keys.add_new(key);
  }

  reset_state_usages(state);

  const Map<SimPointsKey, PinnedPositions> pinned_positions_map = compute_pinned_positions(
      state, world, applied_geometries, keys);
  const Map<SimPointsKey, PinnedRotations> pinned_rotations_map =
      computed_pinned_rotation_animations(state, world, applied_geometries, keys);

  const Map<SimPointsKey, SimPointsWorldProperties> sim_points_props =
      compute_sim_point_world_properties(
          scope, world, keys, applied_geometries, pinned_positions_map, pinned_rotations_map);
  const Map<SimPointsKey, Span<float3>> accelerations_map = compute_external_accelerations(
      scope, world, keys, sim_points_props, applied_geometries);
  const Map<SimPointsKey, Span<float3>> torques_map = compute_external_torques(
      scope, state, world, keys, applied_geometries);

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
  gather_curve_segment_constraints(scope,
                                   state,
                                   world,
                                   applied_geometries,
                                   keys,
                                   sim_points_props,
                                   sub_delta_time,
                                   static_constraint_sets);
  gather_overpressure_constraints(
      scope, state, world, applied_geometries, keys, sim_points_props, static_constraint_sets);
  gather_curves_rod_stretch_and_shear_constraints(scope,
                                                  state,
                                                  world,
                                                  applied_geometries,
                                                  keys,
                                                  sim_points_props,
                                                  sub_delta_time,
                                                  static_constraint_sets);
  gather_curves_rod_bend_and_twist_constraints(scope,
                                               state,
                                               world,
                                               applied_geometries,
                                               keys,
                                               sim_points_props,
                                               sub_delta_time,
                                               static_constraint_sets);
  const Map<SimPointsKey, MutableSpan<float3>> soft_pinned_positions_map =
      gather_soft_pinned_position_constraints(scope,
                                              keys,
                                              sim_points_props,
                                              pinned_positions_map,
                                              sub_delta_time,
                                              static_constraint_sets);
  const Map<SimPointsKey, MutableSpan<math::Quaternion>> soft_pinned_rotations_map =
      gather_soft_pinned_rotation_constraints(scope,
                                              keys,
                                              sim_points_props,
                                              pinned_rotations_map,
                                              sub_delta_time,
                                              static_constraint_sets);
  gather_align_positions_constraints(scope,
                                     world,
                                     applied_geometries,
                                     keys,
                                     sim_points_props,
                                     sub_delta_time,
                                     static_constraint_sets);

  Array<Array<float3>> all_prev_positions(keys.size());
  Array<Array<math::Quaternion>> all_prev_rotations(keys.size());
  for (const int i : keys.index_range()) {
    const SimPoints &sim_points = state.sim_points.lookup(keys[i]);
    all_prev_positions[i].reinitialize(sim_points.points_num);
    if (sim_points.has_rotation) {
      all_prev_rotations[i].reinitialize(sim_points.points_num);
    }
  }

  const Vector<geometry::xpbd_constraint_solver::MutablePointsRef> points_refs =
      prepare_points_refs_for_solver(state, keys);

  for ([[maybe_unused]] const int substep_i : IndexRange(substeps)) {
    const float factor = substeps <= 1 ? 1.0f : float(substep_i) / (substeps - 1);

    /* Remember previous positions and rotations. */
    for (const int i : keys.index_range()) {
      const SimPointsKey &key = keys[i];
      const SimPoints &sim_points = state.sim_points.lookup(key);
      all_prev_positions[i].as_mutable_span().copy_from(sim_points.positions);
      if (sim_points.has_rotation) {
        all_prev_rotations[i].as_mutable_span().copy_from(sim_points.rotations);
      }
    }

    /* Integrate linear and angular velocities. This also applies external forces. */
    if (sub_delta_time > 0.0f) {
      integrate_linear_velocities(state, accelerations_map, sim_points_props, sub_delta_time);
      integrate_angular_velocities(state, torques_map, sim_points_props, sub_delta_time);
    }

    /* Move pinned points to the correct position for the current substep. */
    update_pinned_positions(state, pinned_positions_map, soft_pinned_positions_map, factor);
    update_pinned_rotations(state, pinned_rotations_map, soft_pinned_rotations_map, factor);

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
      update_linear_velocities(state, keys, all_prev_positions, sub_delta_time);
      /* Update angular velocities based on previous and new rotations. */
      update_angular_velocities(state, keys, all_prev_rotations, sub_delta_time);
    }
  }

  remove_unused_states(state);
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
