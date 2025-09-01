/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_bvhutils.hh"
#include "BKE_curves.hh"
#include "BKE_instances.hh"

#include "BLI_array_utils.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_kdtree.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"
#include "BLI_ordered_edge.hh"

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "GEO_reverse_uv_sampler.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"

#include "GEO_xpbd_constraint_sets_common.hh"

#include "node_geometry_util.hh"

#define PROFILE_FUNCTION BLI_NOINLINE

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
  types.append(PressureXPBDConstraintBundle::get_bundle_type());
  types.append(DampingBundle::get_bundle_type());
  types.append(TorqueBundle::get_bundle_type());
  types.append(PinnedRotationXPBDConstraintBundle::get_bundle_type());
  types.append(RodStretchAndShearXPBDConstraintBundle::get_bundle_type());
  types.append(RodBendAndTwistXPBDConstraintBundle::get_bundle_type());
  types.append(AlignPositionsConstraintBundle::get_bundle_type());
  types.append(AttachUVSurfaceConstraintBundle::get_bundle_type());
  types.append(DistanceBasedEdgeBendingConstraintBundle::get_bundle_type());
  types.append(ColliderBundle::get_bundle_type());

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
      .description("Simulation world description that is simulated")
      .field_on_all()
      .structure_type(StructureType::Single);
  b.add_output<decl::Bundle>("World")
      .pass_through_input_index(1)
      .align_with_previous()
      .description("Simulated world");
  b.add_input<decl::Float>("Delta Time").min(0).default_value(1 / 25.0f);

  auto &panel = b.add_panel("Solver");
  panel.add_input<decl::Menu>("Solver Type")
      .static_items(solver_type_items)
      .default_value(SolverType::ParallelGaussSeidel);
  panel.add_input<decl::Int>("Substeps").default_value(10).min(1);
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

class ThreadLocalStorage {
 private:
  struct Item {
    ResourceScope scope;
  };

  threading::EnumerableThreadSpecific<Item> items_;

 public:
  ResourceScope &local_resource_scope()
  {
    return items_.local().scope;
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

struct PinPositionsState {
  struct Item {
    float3 position;
    bool used = true;
  };

  Map<int, Item> positions;
};

struct CurveSegmentRestLengthsState {
 private:
  /**
   * The segment length for the segment after each point.
   * For curve-end-points in non-cyclic curves, this is the distance to the first point in the
   * curve. For single-point curves, this is zero.
   */
  Vector<float> rest_lengths_;

 public:
  CurveSegmentRestLengthsState(const bke::CurvesGeometry &curves)
  {
    const int points_num = curves.points_num();
    const int curves_num = curves.curves_num();
    rest_lengths_.resize(points_num);
    const Span<float3> positions = curves.positions();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    threading::parallel_for(IndexRange(curves_num), 128, [&](const IndexRange curves_range) {
      for (const int curve_i : curves_range) {
        const IndexRange points = points_by_curve[curve_i];
        for (const int point_i : points.drop_back(1)) {
          rest_lengths_[point_i] = math::distance(positions[point_i], positions[point_i + 1]);
        }
        /* The cyclic length is computed even if it might not be needed. */
        rest_lengths_[points.last()] = math::distance(positions[points.last()],
                                                      positions[points.first()]);
      }
    });
  }

  Span<float> rest_lengths() const
  {
    return rest_lengths_;
  }
};

struct CurveSegmentRelativeRestRotationsState {
 private:
  Vector<math::Quaternion> rest_rotations_;

 public:
  CurveSegmentRelativeRestRotationsState(const bke::CurvesGeometry &curves,
                                         const Span<math::Quaternion> rotations)
  {
    const int points_num = curves.points_num();
    const int curves_num = curves.curves_num();
    rest_rotations_.resize(points_num);
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    threading::parallel_for(IndexRange(curves_num), 128, [&](const IndexRange curves_range) {
      for (const int curve_i : curves_range) {
        const IndexRange points = points_by_curve[curve_i];
        for (const int point_i : points.drop_back(1)) {
          const math::Quaternion &r0 = rotations[point_i];
          const math::Quaternion &r1 = rotations[point_i + 1];
          const math::Quaternion relative = math::invert_normalized(r0) * r1;
          rest_rotations_[point_i] = relative;
        }
        /* Cyclic element. */
        const math::Quaternion &r0 = rotations[points.last()];
        const math::Quaternion &r1 = rotations[points.first()];
        const math::Quaternion relative = math::invert_normalized(r0) * r1;
        rest_rotations_[points.last()] = relative;
      }
    });
  }

  Span<math::Quaternion> rest_rotations() const
  {
    return rest_rotations_;
  }
};

struct ExternalColliderKey {
  std::string path;
  Vector<int> ids;

  uint64_t hash() const
  {
    return get_default_hash(this->path, this->ids);
  }

  BLI_STRUCT_EQUALITY_OPERATORS_2(ExternalColliderKey, path, ids)
};

struct ExternalColliderState {
  float4x4 transform;
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
  Map<SimPointsKey, float> initial_volumes;
  Map<ExternalColliderKey, ExternalColliderState> external_colliders;

  mutable Mutex curve_segment_rest_lengths_mutex;
  mutable Map<SimPointsKey, std::unique_ptr<CurveSegmentRestLengthsState>>
      curve_segment_rest_lengths;

  mutable Mutex curve_segment_relative_rest_rotations_mutex;
  mutable Map<SimPointsKey, std::unique_ptr<CurveSegmentRelativeRestRotationsState>>
      curve_segment_relative_rest_rotations;

  int total_points_num() const
  {
    int count = 0;
    for (const SimPoints &sim_points : this->sim_points.values()) {
      count += sim_points.points_num;
    }
    return count;
  }

  bool is_initialization() const
  {
    return this->update_counter == 0;
  }

  Span<float> ensure_curve_segment_rest_lengths(const SimPointsKey &key,
                                                const bke::CurvesGeometry &curves) const
  {
    std::lock_guard<Mutex> lock(this->curve_segment_rest_lengths_mutex);
    return this->curve_segment_rest_lengths
        .lookup_or_add_cb(key,
                          [&]() {
                            return threading::isolate_task([&]() {
                              return std::make_unique<CurveSegmentRestLengthsState>(curves);
                            });
                          })
        ->rest_lengths();
  }

  Span<math::Quaternion> ensure_curve_segment_relative_rest_rotations(
      const SimPointsKey &key, const bke::CurvesGeometry &curves) const
  {
    std::lock_guard<Mutex> lock(this->curve_segment_relative_rest_rotations_mutex);
    const SimPoints &sim_points = this->sim_points.lookup(key);
    return this->curve_segment_relative_rest_rotations
        .lookup_or_add_cb(key,
                          [&]() {
                            return threading::isolate_task([&]() {
                              return std::make_unique<CurveSegmentRelativeRestRotationsState>(
                                  curves, sim_points.rotations);
                            });
                          })
        ->rest_rotations();
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

struct WorldBundles {
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
  BundleVectorSet<PressureXPBDConstraintBundle> overpressure_constraints;
  BundleVectorSet<DampingBundle> dampings;
  BundleVectorSet<TorqueBundle> torques;
  BundleVectorSet<PinnedRotationXPBDConstraintBundle> pinned_rotation_constraints;
  BundleVectorSet<RodStretchAndShearXPBDConstraintBundle> rod_stretch_and_shear_constraints;
  BundleVectorSet<RodBendAndTwistXPBDConstraintBundle> rod_bend_and_twist_constraints;
  BundleVectorSet<AlignPositionsConstraintBundle> align_position_constraints;
  BundleVectorSet<AttachUVSurfaceConstraintBundle> attach_uv_surface_constraints;
  BundleVectorSet<DistanceBasedEdgeBendingConstraintBundle> distance_based_bending_constraints;
  BundleVectorSet<ColliderBundle> colliders;
};

struct CurveRodStretchAndShearConstraintData {
  int key_i;
  Span<float> compliance_terms;
};

struct CurveRodBendAndTwistConstraintData {
  int key_i;
  Span<float> compliance_terms;
};

struct PinnedPositionConstraintData {
  int key_i;
  const fn::FieldEvaluator *evaluator;
  int position_index;
  int compliance_terms_index;
};

struct PinnedRotationConstraintData {
  int key_i;
  const fn::FieldEvaluator *evaluator;
  int rotation_index;
  int compliance_terms_index;
};

struct EdgeLengthConstraintData {
  int key_i;
  const fn::FieldEvaluator *evaluator;
  int compliance_terms_index;
};

struct CurveSegmentLengthConstraintData {
  int key_i;
  const fn::FieldEvaluator *evaluator;
  int compliance_terms_index;
};

struct AlignPositionsConstraintData {
  struct Item {
    int key_i;
    const fn::FieldEvaluator *evaluator;
    int group_id_index;
    int compliance_index;
  };

  Vector<Item> items;
};

struct AttachUVSurfaceConstraintData {
  int mesh_key_i;
  const fn::FieldEvaluator *mesh_evaluator;
  int uv_map_index;

  int points_key_i;
  const fn::FieldEvaluator *point_evaluator;
  int sample_uv_index;
  int compliance_term_index;
};

struct DistanceBasedEdgeBendingConstraintData {
  int key_i;
  const fn::FieldEvaluator *evaluator;
  int compliance_term_index;
};

struct PressureConstraintData {
  int key_i;
  float pressure;
};

struct ForceFieldsData {
  struct Item {
    fn::FieldEvaluator *evaluator;
    int force_index;
  };

  Vector<Item> items;
};

struct TorqueFieldsData {
  struct Item {
    fn::FieldEvaluator *evaluator;
    int torque_index;
  };

  Vector<Item> items;
};

struct InfinitePlaneColliderData {
  int key_i;
  float3 position;
  float3 normal;
  float friction;
};

struct SphericalSelfCollisionData {
  int key_i;
};

struct ExternelMeshColliderData {
  int key_i;
  ExternalColliderKey collider_key;
  const Mesh *mesh;
  float4x4 transform;
  float friction;
  float compliance;
};

struct SimPointsPropertiesData {
  fn::FieldEvaluator *evaluator;
  MutableSpan<float> inverse_masses;
  MutableSpan<float> frictions;
  MutableSpan<float3> inertias;
  MutableSpan<float3> inverse_inertias;
};

struct FieldEvaluatorKey {
  const bke::GeometryComponent *component;
  AttrDomain domain;
  Field<bool> selection;

  BLI_STRUCT_EQUALITY_OPERATORS_3(FieldEvaluatorKey, component, domain, selection)

  uint64_t hash() const
  {
    return get_default_hash(this->component, this->domain, this->selection ? this->selection : 0);
  }
};

struct WorldPreprocessData {
  Map<FieldEvaluatorKey, fn::FieldEvaluator *> field_evaluators;

  Vector<ForceFieldsData> forces_by_key_i;
  Vector<TorqueFieldsData> torques_by_key_i;
  Vector<SimPointsPropertiesData> sim_points_props_by_key_i;

  Vector<PinnedPositionConstraintData> pinned_position_constraints;
  Vector<PinnedRotationConstraintData> pinned_rotation_constraints;
  Vector<CurveRodStretchAndShearConstraintData> rod_stretch_and_shear_constraints;
  Vector<CurveRodBendAndTwistConstraintData> rod_bend_and_twist_constraints;
  Vector<EdgeLengthConstraintData> edge_length_constraints;
  Vector<CurveSegmentLengthConstraintData> curve_segment_length_constraints;
  Vector<AlignPositionsConstraintData> align_positions_constraints;
  Vector<AttachUVSurfaceConstraintData> attach_uv_surface_constraints;
  Vector<DistanceBasedEdgeBendingConstraintData> distance_based_edge_bending_constraints;
  Vector<PressureConstraintData> pressure_constraints;

  Vector<InfinitePlaneColliderData> infinite_plane_colliders;
  Vector<SphericalSelfCollisionData> spherical_self_collisions;
  Vector<ExternelMeshColliderData> mesh_colliders;
};

static const Field<bool> &get_constant_true_field()
{
  static const Field<bool> field = fn::make_constant_field<bool>(true);
  return field;
}

static fn::FieldEvaluator &get_field_evaluator(ResourceScope &scope,
                                               WorldPreprocessData &world_info,
                                               const bke::GeometryComponent &component,
                                               const bke::AttrDomain domain,
                                               const std::optional<Field<bool>> selection)
{
  FieldEvaluatorKey key{&component, domain, selection ? *selection : get_constant_true_field()};
  return *world_info.field_evaluators.lookup_or_add_cb(key, [&]() {
    auto &field_context = scope.construct<bke::GeometryFieldContext>(component, domain);
    const int domain_size = component.attribute_domain_size(domain);
    auto &evaluator = scope.construct<fn::FieldEvaluator>(field_context, domain_size);
    if (selection) {
      evaluator.set_selection(*selection);
    }
    return &evaluator;
  });
}

static float compute_compliance_factor(const float delta_time)
{
  return math::safe_rcp(pow2f(delta_time));
}

static Field<float> convert_to_compliance_term_field(const Field<float> &compliance_field,
                                                     const float delta_time)
{
  const float compliance_factor = compute_compliance_factor(delta_time);
  static auto prepare_compliance_term_fn = mf::build::SI2_SO<float, float, float>(
      "Prepare Compliance Term", [](const float compliance, const float factor) {
        return std::max(0.0f, compliance * factor);
      });
  return Field<float>(fn::FieldOperation::from(
      prepare_compliance_term_fn, {compliance_field, fn::make_constant_field(compliance_factor)}));
}

static Field<float> convert_to_inverse_mass_field(const Field<float> &mass_field)
{
  static auto convert_to_inverse_mass_fn = mf::build::SI1_SO<float, float>(
      "Convert to Inverse Mass", [](const float mass) {
        if (mass <= 0.0f) {
          return 0.0f;
        }
        return 1.0f / mass;
      });
  return Field<float>(fn::FieldOperation::from(convert_to_inverse_mass_fn, {mass_field}));
}

static Field<float> convert_to_clamped_friction_field(const Field<float> &friction_field)
{
  static auto convert_to_clamped_friction_fn = mf::build::SI1_SO<float, float>(
      "Convert to Clamped Friction", [](const float friction) {
        if (friction <= 0.0f) {
          return 0.0f;
        }
        return friction;
      });
  return Field<float>(fn::FieldOperation::from(convert_to_clamped_friction_fn, {friction_field}));
}

static Field<float3> convert_to_inverse_inertia_field(const Field<float3> &inertia_field)
{
  static auto convert_to_inverse_inertia_fn = mf::build::SI1_SO<float3, float3>(
      "Convert to Inverse Inertia", [](const float3 inertia) { return math::safe_rcp(inertia); });
  return Field<float3>(fn::FieldOperation::from(convert_to_inverse_inertia_fn, {inertia_field}));
}

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
  Vector<float> soft_compliance_terms;
  Vector<StartStopPair<float3>> soft_animations;
};

struct PinnedRotations {
  Vector<int> hard_indices;
  Vector<StartStopPair<math::Quaternion>> hard_animations;

  Vector<int> soft_indices;
  Vector<float> soft_compliance_terms;
  Vector<StartStopPair<math::Quaternion>> soft_animations;
};

static AttrDomain get_simulation_domain(const bke::GeometryComponent::Type type)
{
  return type == bke::GeometryComponent::Type::Instance ? AttrDomain::Instance : AttrDomain::Point;
}

template<typename T>
static void parse_bundle(HandleNestedBundleParams &params,
                         BundleParseErrors errors,
                         WorldBundles::BundleVectorSet<T> &r_world_bundles)
{
  if (params.type != T::name) {
    return;
  }
  std::optional<T> parsed_bundle = T::parse(params.bundle, errors);
  if (!parsed_bundle) {
    return;
  }
  parsed_bundle->self_path = Bundle::combine_path(params.path);
  r_world_bundles.add_new(std::move(*parsed_bundle));
}

PROFILE_FUNCTION static WorldBundles parse_world(const Bundle &world_bundle)
{
  WorldBundles world_bundles;
  nested_bundle_foreach(world_bundle, [&](HandleNestedBundleParams &params) {
    BundleParseErrors errors;
    parse_bundle(params, errors, world_bundles.forces);
    parse_bundle(params, errors, world_bundles.gravities);
    parse_bundle(params, errors, world_bundles.geometries);
    parse_bundle(params, errors, world_bundles.edge_length_constraints);
    parse_bundle(params, errors, world_bundles.curve_segment_constraints);
    parse_bundle(params, errors, world_bundles.pinned_position_constraints);
    parse_bundle(params, errors, world_bundles.infinite_ground_planes);
    parse_bundle(params, errors, world_bundles.spherical_self_collision_constraints);
    parse_bundle(params, errors, world_bundles.overpressure_constraints);
    parse_bundle(params, errors, world_bundles.dampings);
    parse_bundle(params, errors, world_bundles.torques);
    parse_bundle(params, errors, world_bundles.pinned_rotation_constraints);
    parse_bundle(params, errors, world_bundles.rod_stretch_and_shear_constraints);
    parse_bundle(params, errors, world_bundles.rod_bend_and_twist_constraints);
    parse_bundle(params, errors, world_bundles.align_position_constraints);
    parse_bundle(params, errors, world_bundles.attach_uv_surface_constraints);
    parse_bundle(params, errors, world_bundles.distance_based_bending_constraints);
    parse_bundle(params, errors, world_bundles.colliders);
  });
  return world_bundles;
}

PROFILE_FUNCTION static void integrate_linear_velocities(const float delta_time,
                                                         const IndexRange range,
                                                         const Span<float3> old_positions,
                                                         const Span<float3> accelerations,
                                                         const SimPointsWorldProperties &props,
                                                         MutableSpan<float3> new_positions,
                                                         MutableSpan<float3> velocities)
{
  const float linear_damping_factor = std::max(1.0f - props.linear_damping * delta_time, 0.0f);
  for (const int i : range.index_range()) {
    const int point_i = range[i];
    const float3 &acceleration = accelerations[point_i];
    velocities[i] += acceleration * delta_time;
    velocities[i] *= linear_damping_factor;
    new_positions[i] = old_positions[i] + velocities[i] * delta_time;
  }
}

PROFILE_FUNCTION static void integrate_angular_velocities(
    const float delta_time,
    const IndexRange range,
    const std::optional<Span<float3>> torques,
    const SimPointsWorldProperties &props,
    const Span<math::Quaternion> old_rotations,
    MutableSpan<math::Quaternion> new_rotations,
    MutableSpan<float3> angular_velocities)
{
  /* Approximation of exponential decay. */
  const float angular_damping_factor = std::max(1.0f - props.angular_damping * delta_time, 0.0f);

  for (const int i : range.index_range()) {
    const int point_i = range[i];
    const float3 &external_torque = torques.has_value() ? (*torques)[point_i] : float3(0.0f);
    const float3 &inertia = props.inertias[i];
    const float3 &inverse_inertia = props.inverse_inertias[point_i];
    if (math::is_zero(inverse_inertia)) {
      continue;
    }
    float3 &angular_velocity = angular_velocities[i];
    const float3 precession = math::cross(angular_velocity, angular_velocity * inertia);
    angular_velocity += delta_time * (external_torque - precession) * inverse_inertia;
    angular_velocity *= angular_damping_factor;
    const math::Quaternion &old_rotation = old_rotations[i];
    const math::Quaternion direction = old_rotation * math::Quaternion(0, angular_velocity);
    new_rotations[i] = math::normalize(
        math::Quaternion(float4(old_rotation) + delta_time * 0.5f * float4(direction)));
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
  array_utils::copy<math::Quaternion>(sim_points.rotations, attribute.span);
  attribute.finish();
}

PROFILE_FUNCTION static void apply_simulation_to_mesh(const XPBDGeometryBundle &bundle,
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
  array_utils::copy<float3>(sim_points.positions, mesh_positions);
  mesh->tag_positions_changed();

  store_rotation_if_necessary(bundle, sim_points, AttrDomain::Point, mesh->attributes_for_write());
}

PROFILE_FUNCTION static void apply_simulation_to_pointcloud(const XPBDGeometryBundle &bundle,
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
  array_utils::copy<float3>(sim_points.positions, pointcloud_positions);
  pointcloud->tag_positions_changed();

  store_rotation_if_necessary(
      bundle, sim_points, AttrDomain::Point, pointcloud->attributes_for_write());
}

PROFILE_FUNCTION static void apply_simulation_to_curves(const XPBDGeometryBundle &bundle,
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
  array_utils::copy<float3>(sim_points.positions, curves_positions);
  curves.tag_positions_changed();

  store_rotation_if_necessary(
      bundle, sim_points, AttrDomain::Point, curves.attributes_for_write());
}

PROFILE_FUNCTION static void apply_simulation_to_instances(const XPBDGeometryBundle &bundle,
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

PROFILE_FUNCTION static void update_xpbd_state_for_geometry(
    XPBDState &state,
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
      state.curve_segment_rest_lengths.remove(key);
      state.curve_segment_relative_rest_rotations.remove(key);

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

PROFILE_FUNCTION static Map<SimPointsKey, Span<float3>> compute_external_accelerations(
    ThreadLocalStorage &tls,
    const WorldBundles &world_bundles,
    const WorldPreprocessData &world_info,
    const XPBDState &state,
    const Span<SimPointsKey> keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props)
{
  ResourceScope &scope = tls.local_resource_scope();
  float3 gravity(0.0f);
  for (const GravityBundle &gravity_bundle : world_bundles.gravities) {
    gravity = gravity_bundle.gravity;
  }
  Map<SimPointsKey, Span<float3>> accelerations_map;
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const SimPoints &sim_points = state.sim_points.lookup(key);
    const Span<float> inverse_masses = sim_points_props.lookup(key).inverse_masses;
    const int points_num = sim_points.points_num;

    /* Initially this is the sums of forces and then the acceleration. */
    MutableSpan<float3> result = scope.allocator().allocate_array<float3>(points_num);
    array_utils::copy(VArray<float3>::from_single(float3(0.0f), points_num), result);

    const ForceFieldsData &forces_info = world_info.forces_by_key_i[key_i];
    for (const ForceFieldsData::Item &item : forces_info.items) {
      const IndexMask &mask = item.evaluator->get_evaluated_selection_as_mask();
      const VArray<float3> force = item.evaluator->get_evaluated<float3>(item.force_index);
      mask.foreach_index(GrainSize(1024), [&](const int i) { result[i] += force[i]; });
    }

    threading::parallel_for(IndexRange(points_num), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        const float inverse_mass = inverse_masses[i];
        const float3 &force = result[i];
        const float3 acceleration = force * inverse_mass + gravity;
        result[i] = acceleration;
      }
    });

    accelerations_map.add_new(key, result);
  }
  return accelerations_map;
}

PROFILE_FUNCTION static Map<SimPointsKey, Span<float3>> compute_external_torques(
    ThreadLocalStorage &tls,
    const XPBDState &state,
    const WorldPreprocessData &world_info,
    const Span<SimPointsKey> keys)
{
  ResourceScope &scope = tls.local_resource_scope();
  Map<SimPointsKey, Span<float3>> torques_map;
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const SimPoints &sim_points = state.sim_points.lookup(key);
    if (!sim_points.has_rotation) {
      continue;
    }
    const int points_num = sim_points.points_num;

    MutableSpan<float3> result = scope.allocator().allocate_array<float3>(points_num);
    result.fill(float3(0.0f));

    const TorqueFieldsData &torques_info = world_info.torques_by_key_i[key_i];
    for (const TorqueFieldsData::Item &item : torques_info.items) {
      const IndexMask &mask = item.evaluator->get_evaluated_selection_as_mask();
      const VArray<float3> &torques_varray = item.evaluator->get_evaluated<float3>(
          item.torque_index);
      mask.foreach_index(GrainSize(1024), [&](const int i) { result[i] += torques_varray[i]; });
    }

    torques_map.add(key, result);
  }
  return torques_map;
}

PROFILE_FUNCTION static Map<SimPointsKey, SimPointsWorldProperties>
compute_sim_point_world_properties(const WorldBundles &world_bundles,
                                   const WorldPreprocessData &world_info,
                                   const Span<SimPointsKey> keys,
                                   const Span<GeometrySet> applied_geometries,
                                   const Map<SimPointsKey, PinnedPositions> &pinned_positions_map,
                                   const Map<SimPointsKey, PinnedRotations> &pinned_rotations_map)
{
  Map<SimPointsKey, SimPointsWorldProperties> properties_map;
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_i];
    const PinnedPositions *pinned_positions = pinned_positions_map.lookup_ptr(key);
    const PinnedRotations *pinned_rotations = pinned_rotations_map.lookup_ptr(key);

    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometry.get_component(type);
    if (!component) {
      continue;
    }

    const SimPointsPropertiesData &props_info = world_info.sim_points_props_by_key_i[key_i];

    /* Set mass of pinned points to infinity (i.e. the inverse mass is 0). */
    if (pinned_positions) {
      threading::parallel_for(
          pinned_positions->hard_indices.index_range(), 4096, [&](const IndexRange range) {
            for (const int i : pinned_positions->hard_indices.as_span().slice(range)) {
              props_info.inverse_masses[i] = 0.0f;
            }
          });
    }

    /* Set inertia of pinned rotations to infinity (i.e. the inverse inertia is 0). */
    if (pinned_rotations) {
      threading::parallel_for(
          pinned_rotations->hard_indices.index_range(), 4096, [&](const IndexRange range) {
            for (const int i : pinned_rotations->hard_indices.as_span().slice(range)) {
              props_info.inertias[i] = float3(std::numeric_limits<float>::infinity());
              props_info.inverse_inertias[i] = float3(0.0f);
            }
          });
    }

    const Vector damping_bundles = filter_bundles_for_path<DampingBundle>(world_bundles.dampings,
                                                                          key.path);
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    for (const DampingBundle *damping_bundle : damping_bundles) {
      linear_damping += damping_bundle->linear_damping;
      angular_damping += damping_bundle->angular_damping;
    }

    properties_map.add_new(key,
                           {props_info.inverse_masses,
                            props_info.frictions,
                            props_info.inertias,
                            props_info.inverse_inertias,
                            linear_damping,
                            angular_damping});
  }
  return properties_map;
}

PROFILE_FUNCTION static Span<float> prepare_distance_constraint_lengths(
    ResourceScope &scope,
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

PROFILE_FUNCTION static Vector<xpbd::DistanceConstraintSet *> gather_edge_length_constraints(
    ResourceScope &scope,
    XPBDState &state,
    const WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys)
{
  Vector<xpbd::DistanceConstraintSet *> result;
  for (const EdgeLengthConstraintData &constraint : world_info.edge_length_constraints) {
    const int key_i = constraint.key_i;
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];

    const Mesh &mesh = *applied_geometry.get_mesh();
    const Span<float3> mesh_positions = mesh.vert_positions();
    const Span<int2> mesh_edges = mesh.edges();

    const IndexMask &edge_mask = constraint.evaluator->get_evaluated_selection_as_mask();
    if (edge_mask.is_empty()) {
      continue;
    }

    Span<int2> constraint_edges;
    if (edge_mask.size() == mesh_edges.size()) {
      constraint_edges = mesh_edges;
    }
    else {
      MutableSpan<int2> masked_edges = scope.allocator().allocate_array<int2>(edge_mask.size());
      array_utils::gather(mesh_edges, edge_mask, masked_edges);
      constraint_edges = masked_edges;
    }

    MutableSpan<float> compliance_terms = scope.allocator().allocate_array<float>(
        edge_mask.size());
    constraint.evaluator->get_evaluated<float>(constraint.compliance_terms_index)
        .materialize_compressed_to_uninitialized(edge_mask, compliance_terms);

    const Span<float> constraint_lengths = prepare_distance_constraint_lengths(
        scope, state, mesh_positions, key, constraint_edges);
    result.append(&scope.construct<xpbd::DistanceConstraintSet>(
        key_i, constraint_edges, constraint_lengths, compliance_terms));
  }
  return result;
}

PROFILE_FUNCTION static Vector<xpbd::DistanceConstraintSet *> gather_curve_segment_constraints(
    ResourceScope &scope,
    XPBDState &state,
    const WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys)
{
  Vector<xpbd::DistanceConstraintSet *> result;
  for (const CurveSegmentLengthConstraintData &constraint :
       world_info.curve_segment_length_constraints)
  {
    const int key_i = constraint.key_i;
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];

    const Curves &curves_id = *applied_geometry.get_curves();
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const VArray<bool> cyclics = curves.cyclic();

    const VArray<float> compliance_terms = constraint.evaluator->get_evaluated<float>(
        constraint.compliance_terms_index);

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
        constraint_segments.append({point_i, next_point_i});
        constraint_compliance_terms.append(compliance_terms[point_i]);
      }
      const bool cyclic = cyclics[curve_i];
      if (cyclic) {
        const int point_i = points.last();
        const int next_point_i = points.first();
        constraint_segments.append({point_i, next_point_i});
        constraint_compliance_terms.append(compliance_terms[point_i]);
      }
    }

    const Span<float> constraint_lengths = prepare_distance_constraint_lengths(
        scope, state, curves.positions(), key, constraint_segments);

    result.append(&scope.construct<xpbd::DistanceConstraintSet>(
        key_i, constraint_segments, constraint_lengths, constraint_compliance_terms));
  }
  return result;
}

PROFILE_FUNCTION static Vector<xpbd::PressureConstraintSet *> gather_pressure_constraints(
    ResourceScope &scope,
    XPBDState &state,
    const WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys)
{
  Vector<xpbd::PressureConstraintSet *> result;
  for (const PressureConstraintData &constraint : world_info.pressure_constraints) {
    const SimPointsKey &key = keys[constraint.key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const Mesh &mesh = *applied_geometry.get_mesh();
    const Span<int3> tris = mesh.corner_tris();
    const Span<int> corner_verts = mesh.corner_verts();
    const Span<float3> positions = mesh.vert_positions();

    const float initial_volume = state.initial_volumes.lookup_or_add_cb(key, [&]() {
      return xpbd::PressureConstraintSet::compute_volume(tris, corner_verts, positions);
    });
    if (initial_volume <= 0.0f) {
      continue;
    }
    result.append(&scope.construct<xpbd::PressureConstraintSet>(
        constraint.key_i, tris, corner_verts, constraint.pressure, initial_volume));
  }
  return result;
}

PROFILE_FUNCTION static void prepare_evaluation__forces(ResourceScope &scope,
                                                        WorldPreprocessData &world_info,
                                                        const WorldBundles &world_bundles,
                                                        const Span<GeometrySet> applied_geometries,
                                                        const VectorSet<SimPointsKey> &keys)
{
  world_info.forces_by_key_i.resize(keys.size());
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    Vector force_bundles = filter_bundles_for_path<ForceBundle>(world_bundles.forces, key.path);
    if (force_bundles.is_empty()) {
      continue;
    }
    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometry.get_component(type);
    if (!component) {
      continue;
    }
    const bke::AttrDomain domain = get_simulation_domain(type);
    ForceFieldsData &force_fields_info = world_info.forces_by_key_i[key_i];
    for (const ForceBundle *force_bundle : force_bundles) {
      fn::FieldEvaluator &evaluator = get_field_evaluator(
          scope, world_info, *component, domain, force_bundle->selection);
      force_fields_info.items.append({&evaluator, evaluator.add(force_bundle->force)});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__torques(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const XPBDState &state,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys)
{
  world_info.torques_by_key_i.resize(keys.size());
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
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
    Vector used_torques = filter_bundles_for_path<TorqueBundle>(world_bundles.torques, key.path);
    if (used_torques.is_empty()) {
      continue;
    }
    const AttrDomain domain = get_simulation_domain(type);
    TorqueFieldsData &torque_fields_info = world_info.torques_by_key_i[key_i];
    for (const TorqueBundle *torque_bundle : used_torques) {
      fn::FieldEvaluator &evaluator = get_field_evaluator(
          scope, world_info, *component, domain, torque_bundle->selection);
      torque_fields_info.items.append({&evaluator, evaluator.add(torque_bundle->torque)});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__sim_points_props(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const XPBDState &state,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys)
{
  world_info.sim_points_props_by_key_i.resize(keys.size());
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const XPBDGeometryBundle &geometry_bundle = world_bundles.geometries[geometry_bundle_i];
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const SimPoints &sim_points = state.sim_points.lookup(key);

    const bke::GeometryComponent *component = applied_geometry.get_component(key.type);
    if (!component) {
      continue;
    }

    const AttrDomain domain = get_simulation_domain(key.type);
    SimPointsPropertiesData &sim_points_props_info = world_info.sim_points_props_by_key_i[key_i];
    sim_points_props_info.evaluator = &get_field_evaluator(
        scope, world_info, *component, domain, std::nullopt);

    sim_points_props_info.inverse_masses = scope.allocator().allocate_array<float>(
        sim_points.points_num);
    sim_points_props_info.evaluator->add_with_destination(
        convert_to_inverse_mass_field(geometry_bundle.mass), sim_points_props_info.inverse_masses);

    sim_points_props_info.frictions = scope.allocator().allocate_array<float>(
        sim_points.points_num);
    sim_points_props_info.evaluator->add_with_destination(
        convert_to_clamped_friction_field(geometry_bundle.friction),
        sim_points_props_info.frictions);

    if (geometry_bundle.has_rotation) {
      sim_points_props_info.inertias = scope.allocator().allocate_array<float3>(
          sim_points.points_num);
      sim_points_props_info.evaluator->add_with_destination(geometry_bundle.inertia,
                                                            sim_points_props_info.inertias);

      sim_points_props_info.inverse_inertias = scope.allocator().allocate_array<float3>(
          sim_points.points_num);
      sim_points_props_info.evaluator->add_with_destination(
          convert_to_inverse_inertia_field(geometry_bundle.inertia),
          sim_points_props_info.inverse_inertias);
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__pinned_position_constraints(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const float delta_time)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const Vector constraint_bundles = filter_bundles_for_path<PinnedPositionXPBDConstraintBundle>(
        world_bundles.pinned_position_constraints, key.path);
    if (constraint_bundles.is_empty()) {
      continue;
    }
    const bke::GeometryComponent *component = applied_geometry.get_component(key.type);
    if (!component) {
      continue;
    }
    const AttrDomain domain = get_simulation_domain(key.type);
    for (const PinnedPositionXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      fn::FieldEvaluator &evaluator = get_field_evaluator(
          scope, world_info, *component, domain, constraint_bundle->selection);
      world_info.pinned_position_constraints.append(
          {key_i,
           &evaluator,
           evaluator.add(constraint_bundle->position),
           evaluator.add(
               convert_to_compliance_term_field(constraint_bundle->compliance, delta_time))});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__pinned_rotation_constraints(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const XPBDState &state,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const float delta_time)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const SimPoints &sim_points = state.sim_points.lookup(key);
    if (!sim_points.has_rotation) {
      continue;
    }
    const Vector constraint_bundles = filter_bundles_for_path<PinnedRotationXPBDConstraintBundle>(
        world_bundles.pinned_rotation_constraints, key.path);
    if (constraint_bundles.is_empty()) {
      continue;
    }
    const bke::GeometryComponent *component = applied_geometry.get_component(key.type);
    if (!component) {
      continue;
    }
    const AttrDomain domain = get_simulation_domain(key.type);
    for (const PinnedRotationXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      fn::FieldEvaluator &evaluator = get_field_evaluator(
          scope, world_info, *component, domain, constraint_bundle->selection);
      world_info.pinned_rotation_constraints.append(
          {key_i,
           &evaluator,
           evaluator.add(constraint_bundle->rotation),
           evaluator.add(
               convert_to_compliance_term_field(constraint_bundle->compliance, delta_time))});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__curves_rod_stretch_and_shear_constraints(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const XPBDState &state,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const float delta_time)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Curve) {
      continue;
    }
    const SimPoints &sim_points = state.sim_points.lookup(key);
    if (!sim_points.has_rotation) {
      continue;
    }
    const Vector constraint_bundles =
        filter_bundles_for_path<RodStretchAndShearXPBDConstraintBundle>(
            world_bundles.rod_stretch_and_shear_constraints, key.path);
    if (constraint_bundles.is_empty()) {
      continue;
    }
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const bke::CurveComponent &component = *applied_geometry.get_component<bke::CurveComponent>();

    fn::FieldEvaluator &evaluator = get_field_evaluator(
        scope, world_info, component, bke::AttrDomain::Point, std::nullopt);
    for (const RodStretchAndShearXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      MutableSpan<float> compliance_terms = scope.allocator().allocate_array<float>(
          sim_points.points_num);
      evaluator.add_with_destination(
          convert_to_compliance_term_field(constraint_bundle->compliance, delta_time),
          compliance_terms);
      world_info.rod_stretch_and_shear_constraints.append(
          CurveRodStretchAndShearConstraintData{key_i, compliance_terms});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__edge_length_constraints(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const float delta_time)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Mesh) {
      continue;
    }
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];

    const Vector constraint_bundles = filter_bundles_for_path<EdgeLengthXPBDConstraintBundle>(
        world_bundles.edge_length_constraints, key.path);
    if (constraint_bundles.is_empty()) {
      continue;
    }
    const bke::MeshComponent &mesh_component =
        *applied_geometry.get_component<bke::MeshComponent>();
    for (const EdgeLengthXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      fn::FieldEvaluator &evaluator = get_field_evaluator(
          scope, world_info, mesh_component, bke::AttrDomain::Edge, constraint_bundle->selection);
      world_info.edge_length_constraints.append({key_i,
                                                 &evaluator,
                                                 evaluator.add(convert_to_compliance_term_field(
                                                     constraint_bundle->compliance, delta_time))});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__curve_length_constraints(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const float delta_time)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Curve) {
      continue;
    }
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const bke::CurveComponent &component = *applied_geometry.get_component<bke::CurveComponent>();

    Vector constraint_bundles = filter_bundles_for_path<CurveSegmentXPBDConstraintBundle>(
        world_bundles.curve_segment_constraints, key.path);
    if (constraint_bundles.is_empty()) {
      continue;
    }

    fn::FieldEvaluator &evaluator = get_field_evaluator(
        scope, world_info, component, bke::AttrDomain::Point, std::nullopt);
    for (const CurveSegmentXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      world_info.curve_segment_length_constraints.append(
          {key_i,
           &evaluator,
           evaluator.add(
               convert_to_compliance_term_field(constraint_bundle->compliance, delta_time))});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__align_positions_constraints(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys)
{
  for (const AlignPositionsConstraintBundle &constraint_bundle :
       world_bundles.align_position_constraints)
  {
    const Vector<int> filtered_key_indices = filter_sim_points_keys(
        constraint_bundle.self_path, constraint_bundle.filter, keys);
    if (filtered_key_indices.is_empty()) {
      continue;
    }
    AlignPositionsConstraintData constraint;
    for (const int key_i : filtered_key_indices) {
      const SimPointsKey &key = keys[key_i];
      const bke::GeometryComponent::Type type = key.type;
      const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
      const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
      const bke::GeometryComponent *component = applied_geometry.get_component(type);
      if (!component) {
        continue;
      }
      const bke::AttrDomain domain = get_simulation_domain(type);
      fn::FieldEvaluator &evaluator = get_field_evaluator(
          scope, world_info, *component, domain, constraint_bundle.selection);
      constraint.items.append({key_i,
                               &evaluator,
                               evaluator.add(constraint_bundle.group_id),
                               evaluator.add(constraint_bundle.compliance)});
    }
    if (constraint.items.is_empty()) {
      continue;
    }
    world_info.align_positions_constraints.append(std::move(constraint));
  }
}

PROFILE_FUNCTION static void prepare_evaluation__attach_uv_surface_constraints(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const float delta_time)
{
  for (const AttachUVSurfaceConstraintBundle &constraint_bundle :
       world_bundles.attach_uv_surface_constraints)
  {
    const SimPointsKey mesh_key{constraint_bundle.mesh_path, bke::GeometryComponent::Type::Mesh};
    const int mesh_key_i = keys.index_of_try(mesh_key);
    if (mesh_key_i == -1) {
      continue;
    }
    Vector<int> filtered_points_keys = filter_sim_points_keys(
        constraint_bundle.self_path, constraint_bundle.filter, keys);
    if (filtered_points_keys.is_empty()) {
      continue;
    }
    const int mesh_bundle_i = world_bundles.geometries.index_of_as(mesh_key.path);
    const bke::MeshComponent *mesh_component =
        applied_geometries[mesh_bundle_i].get_component<bke::MeshComponent>();
    if (!mesh_component) {
      continue;
    }

    fn::FieldEvaluator &mesh_evaluator = get_field_evaluator(
        scope, world_info, *mesh_component, bke::AttrDomain::Corner, std::nullopt);

    for (const int points_key_i : filtered_points_keys) {
      AttachUVSurfaceConstraintData constraint;
      constraint.mesh_key_i = mesh_key_i;
      constraint.mesh_evaluator = &mesh_evaluator;
      constraint.uv_map_index = mesh_evaluator.add(constraint_bundle.uv_map);
      const SimPointsKey &points_key = keys[points_key_i];
      const int points_bundle_i = world_bundles.geometries.index_of_as(points_key.path);
      const bke::GeometryComponent *points_component =
          applied_geometries[points_bundle_i].get_component(points_key.type);
      if (!points_component) {
        continue;
      }
      const AttrDomain domain = get_simulation_domain(points_key.type);
      fn::FieldEvaluator &point_evaluator = get_field_evaluator(
          scope, world_info, *points_component, domain, constraint_bundle.selection);
      constraint.points_key_i = points_key_i;
      constraint.point_evaluator = &point_evaluator;
      constraint.sample_uv_index = point_evaluator.add(constraint_bundle.sample_uv);
      constraint.compliance_term_index = point_evaluator.add(
          convert_to_compliance_term_field(constraint_bundle.compliance, delta_time));
      world_info.attach_uv_surface_constraints.append(std::move(constraint));
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__distance_based_edge_bending_constraints(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const float delta_time)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Mesh) {
      continue;
    }
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];

    const Vector constraint_bundles =
        filter_bundles_for_path<DistanceBasedEdgeBendingConstraintBundle>(
            world_bundles.distance_based_bending_constraints, key.path);
    if (constraint_bundles.is_empty()) {
      continue;
    }

    const bke::MeshComponent &mesh_component = *static_cast<const bke::MeshComponent *>(
        applied_geometry.get_component(bke::GeometryComponent::Type::Mesh));
    for (const DistanceBasedEdgeBendingConstraintBundle *constraint_bundle : constraint_bundles) {
      fn::FieldEvaluator &evaluator = get_field_evaluator(
          scope, world_info, mesh_component, bke::AttrDomain::Edge, constraint_bundle->selection);
      world_info.distance_based_edge_bending_constraints.append(
          {key_i,
           &evaluator,
           evaluator.add(
               convert_to_compliance_term_field(constraint_bundle->compliance, delta_time))});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__pressure_constraints(
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Mesh) {
      continue;
    }
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const Mesh &mesh = *applied_geometry.get_mesh();
    if (mesh.faces_num == 0) {
      continue;
    }
    const Vector constraint_bundles = filter_bundles_for_path<PressureXPBDConstraintBundle>(
        world_bundles.overpressure_constraints, key.path);
    for (const PressureXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      world_info.pressure_constraints.append({key_i, constraint_bundle->pressure});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__infinite_plane_colliders(
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const VectorSet<SimPointsKey> &keys)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const Vector constraint_bundles = filter_bundles_for_path<InfiniteGroundPlaneBundle>(
        world_bundles.infinite_ground_planes, key.path);
    for (const InfiniteGroundPlaneBundle *constraint_bundle : constraint_bundles) {
      if (math::is_zero(constraint_bundle->normal)) {
        continue;
      }
      world_info.infinite_plane_colliders.append({key_i,
                                                  constraint_bundle->position,
                                                  constraint_bundle->normal,
                                                  constraint_bundle->friction});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__spherical_self_collisions(
    WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const VectorSet<SimPointsKey> &keys)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const Vector constraint_bundles =
        filter_bundles_for_path<SphericalSelfCollisionXPBDConstraintBundle>(
            world_bundles.spherical_self_collision_constraints, key.path);
    for ([[maybe_unused]] const SphericalSelfCollisionXPBDConstraintBundle *constraint_bundle :
         constraint_bundles)
    {
      world_info.spherical_self_collisions.append({key_i});
    }
  }
}

PROFILE_FUNCTION static void prepare_evaluation__colliders(WorldPreprocessData &world_info,
                                                           const WorldBundles &world_bundles,
                                                           const VectorSet<SimPointsKey> &keys)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    const Vector constraint_bundles = filter_bundles_for_path<ColliderBundle>(
        world_bundles.colliders, key.path);
    for (const ColliderBundle *constraint_bundle : constraint_bundles) {
      if (const Mesh *mesh = constraint_bundle->geometry.get_mesh()) {
        world_info.mesh_colliders.append({key_i,
                                          ExternalColliderKey{constraint_bundle->self_path},
                                          mesh,
                                          float4x4::identity(),
                                          constraint_bundle->friction,
                                          constraint_bundle->compliance});
      }
      if (const bke::Instances *instances = constraint_bundle->geometry.get_instances()) {
        const Span<float4x4> transforms = instances->transforms();
        const Span<bke::InstanceReference> references = instances->references();
        const Span<int> handles = instances->reference_handles();
        const Span<int> instance_ids = instances->unique_ids();
        for (const int instance_i : transforms.index_range()) {
          const int handle = handles[instance_i];
          if (!references.index_range().contains(handle)) {
            continue;
          }
          const int instance_id = instance_ids[instance_i];
          const bke::InstanceReference &reference = references[handle];
          GeometrySet reference_geo;
          reference.to_geometry_set(reference_geo);
          if (const Mesh *mesh = reference_geo.get_mesh()) {
            world_info.mesh_colliders.append(
                {key_i,
                 ExternalColliderKey{constraint_bundle->self_path, {instance_id}},
                 mesh,
                 transforms[instance_i],
                 constraint_bundle->friction,
                 constraint_bundle->compliance});
          }
        }
      }
    }
  }
}

PROFILE_FUNCTION static Vector<xpbd::RodStretchAndShearCurveLocalConstraintSet *>
build_constraint_sets__curves_rod_stretch_and_shear(ThreadLocalStorage &tls,
                                                    const WorldPreprocessData &world_info,
                                                    const XPBDState &state,
                                                    const WorldBundles &world_bundles,
                                                    const Span<GeometrySet> applied_geometries,
                                                    const VectorSet<SimPointsKey> &keys)
{
  ResourceScope &scope = tls.local_resource_scope();
  Vector<xpbd::RodStretchAndShearCurveLocalConstraintSet *> result;
  for (const CurveRodStretchAndShearConstraintData &constraint_info :
       world_info.rod_stretch_and_shear_constraints)
  {
    const int key_i = constraint_info.key_i;
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const Curves &curves_id = *applied_geometry.get_curves();
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const Span<float> rest_lengths = state.ensure_curve_segment_rest_lengths(key, curves);
    result.append(&scope.construct<xpbd::RodStretchAndShearCurveLocalConstraintSet>(
        key_i, points_by_curve, rest_lengths, constraint_info.compliance_terms));
  }
  return result;
}

PROFILE_FUNCTION static void prepare_evaluation__curves_rod_bend_and_twist_constraints(
    ResourceScope &scope,
    WorldPreprocessData &world_info,
    const XPBDState &state,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const VectorSet<SimPointsKey> &keys,
    const float delta_time)
{
  for (const int key_i : keys.index_range()) {
    const SimPointsKey &key = keys[key_i];
    if (key.type != bke::GeometryComponent::Type::Curve) {
      continue;
    }
    const SimPoints &sim_points = state.sim_points.lookup(key);
    if (!sim_points.has_rotation) {
      continue;
    }
    const Vector constraint_bundles = filter_bundles_for_path<RodBendAndTwistXPBDConstraintBundle>(
        world_bundles.rod_bend_and_twist_constraints, key.path);
    if (constraint_bundles.is_empty()) {
      continue;
    }
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const bke::CurveComponent &component = *applied_geometry.get_component<bke::CurveComponent>();

    fn::FieldEvaluator &evaluator = get_field_evaluator(
        scope, world_info, component, bke::AttrDomain::Point, std::nullopt);
    for (const RodBendAndTwistXPBDConstraintBundle *constraint_bundle : constraint_bundles) {
      MutableSpan<float> compliance_terms = scope.allocator().allocate_array<float>(
          evaluator.evaluation_mask().min_array_size());
      evaluator.add_with_destination(
          convert_to_compliance_term_field(constraint_bundle->compliance, delta_time),
          compliance_terms);
      world_info.rod_bend_and_twist_constraints.append(
          CurveRodBendAndTwistConstraintData{key_i, compliance_terms});
    }
  }
}

PROFILE_FUNCTION static Vector<xpbd::RodBendAndTwistCurveLocalConstraintSet *>
build_constraint_sets__curves_rod_bend_and_twist(ThreadLocalStorage &tls,
                                                 const WorldPreprocessData &world_info,
                                                 const XPBDState &state,
                                                 const WorldBundles &world_bundles,
                                                 const Span<GeometrySet> applied_geometries,
                                                 const VectorSet<SimPointsKey> &keys)
{
  ResourceScope &scope = tls.local_resource_scope();
  Vector<xpbd::RodBendAndTwistCurveLocalConstraintSet *> result;
  for (const CurveRodBendAndTwistConstraintData &constraint_info :
       world_info.rod_bend_and_twist_constraints)
  {
    const int key_i = constraint_info.key_i;
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const Curves &curves_id = *applied_geometry.get_curves();
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const Span<math::Quaternion> rest_rotations =
        state.ensure_curve_segment_relative_rest_rotations(key, curves);
    result.append(&scope.construct<xpbd::RodBendAndTwistCurveLocalConstraintSet>(
        key_i, points_by_curve, rest_rotations, constraint_info.compliance_terms));
  }
  return result;
}

PROFILE_FUNCTION static Map<SimPointsKey, MutableSpan<float3>>
gather_soft_pinned_position_constraints(
    ResourceScope &scope,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, PinnedPositions> &pinned_positions_map,
    xpbd::ConstraintSetCollector &r_constraints)
{
  Map<SimPointsKey, MutableSpan<float3>> result;
  for (const auto item : pinned_positions_map.items()) {
    const SimPointsKey &key = item.key;
    const int key_i = keys.index_of(key);
    const PinnedPositions &pinned_positions = item.value;
    const int constraints_num = pinned_positions.soft_indices.size();
    if (constraints_num == 0) {
      continue;
    }

    /* Positions are initialized in #update_pinned_positions. */
    MutableSpan<float3> soft_pinned_positions = scope.allocator().allocate_array<float3>(
        constraints_num);
    result.add(key, soft_pinned_positions);

    r_constraints.general.append(&scope.construct<xpbd::PinnedPositionConstraintSet>(
        key_i,
        pinned_positions.soft_indices,
        soft_pinned_positions,
        pinned_positions.soft_compliance_terms));
  }
  return result;
}

static Map<SimPointsKey, MutableSpan<math::Quaternion>> gather_soft_pinned_rotation_constraints(
    ResourceScope &scope,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, PinnedRotations> &pinned_rotations_map,
    xpbd::ConstraintSetCollector &r_constraints)
{
  Map<SimPointsKey, MutableSpan<math::Quaternion>> result;
  for (const auto item : pinned_rotations_map.items()) {
    const SimPointsKey &key = item.key;
    const int key_i = keys.index_of(key);
    const PinnedRotations &pinned_rotations = item.value;
    const int constraints_num = pinned_rotations.soft_indices.size();
    if (constraints_num == 0) {
      continue;
    }

    /* Rotations are initialized in #update_pinned_rotations. */
    MutableSpan<math::Quaternion> soft_pinned_rotations =
        scope.allocator().allocate_array<math::Quaternion>(constraints_num);
    result.add(key, soft_pinned_rotations);

    r_constraints.general.append(
        &scope.construct<xpbd::PinRotationConstraintSet>(key_i,
                                                         pinned_rotations.soft_indices,
                                                         soft_pinned_rotations,
                                                         pinned_rotations.soft_compliance_terms));
  }
  return result;
}

PROFILE_FUNCTION static Vector<xpbd::AlignPositionsConstraintSet *>
gather_align_positions_constraints(ResourceScope &scope,
                                   const WorldPreprocessData &world_info,
                                   const float delta_time)
{
  const float compliance_factor = compute_compliance_factor(delta_time);
  Vector<xpbd::AlignPositionsConstraintSet *> result;
  for (const AlignPositionsConstraintData &constraint : world_info.align_positions_constraints) {
    struct PointInfo {
      int key_i;
      int point_i;
      float compliance;
    };
    MultiValueMap<int, PointInfo> infos_by_group_id;
    for (const AlignPositionsConstraintData::Item &item : constraint.items) {
      const int key_i = item.key_i;
      const IndexMask &mask = item.evaluator->get_evaluated_selection_as_mask();
      if (mask.is_empty()) {
        continue;
      }
      const VArray<int> group_ids = item.evaluator->get_evaluated<int>(item.group_id_index);
      const VArray<float> compliances = item.evaluator->get_evaluated<float>(
          item.compliance_index);
      mask.foreach_index([&](const int point_i) {
        const int group_id = group_ids[point_i];
        const float compliance = compliances[point_i];
        infos_by_group_id.add(group_id, {key_i, point_i, compliance});
      });
    }

    Vector<int> &offsets = scope.construct<Vector<int>>();
    Vector<int> &constraint_geo_indices = scope.construct<Vector<int>>();
    Vector<int> &constraint_point_indices = scope.construct<Vector<int>>();
    Vector<float> &constraint_compliance_terms = scope.construct<Vector<float>>();

    offsets.append(0);

    for (const Span<PointInfo> point_infos : infos_by_group_id.values()) {
      if (point_infos.size() <= 1) {
        /* The constraint needs at least two points. */
        continue;
      }

      float compliances_log_sum = 0.0f;
      for (const PointInfo &point_info : point_infos) {
        const float point_compliance = std::max(0.0f, point_info.compliance);
        if (point_compliance > 0.0f) {
          compliances_log_sum += logf(point_compliance);
        }
      }
      const float compliances_log_mean = compliances_log_sum / point_infos.size();
      const float compliance = expf(compliances_log_mean);
      const float compliance_term = compliance * compliance_factor;

      for (const PointInfo &point_info : point_infos) {
        constraint_geo_indices.append(point_info.key_i);
        constraint_point_indices.append(point_info.point_i);
      }
      constraint_compliance_terms.append(compliance_term);
      offsets.append(constraint_point_indices.size());
    }

    OffsetIndices<int> offset_indices(offsets);
    if (offset_indices.is_empty()) {
      continue;
    }

    result.append(&scope.construct<xpbd::AlignPositionsConstraintSet>(offset_indices,
                                                                      constraint_compliance_terms,
                                                                      constraint_geo_indices,
                                                                      constraint_point_indices));
  }
  return result;
}

PROFILE_FUNCTION static Vector<xpbd::AttachUVSurfaceConstraintSet *>
gather_attach_uv_surface_constraints(ResourceScope &scope,
                                     const WorldPreprocessData &world_info,
                                     const WorldBundles &world_bundles,
                                     const VectorSet<SimPointsKey> &keys)
{
  Vector<xpbd::AttachUVSurfaceConstraintSet *> result;
  for (const AttachUVSurfaceConstraintData &constraint : world_info.attach_uv_surface_constraints)
  {
    const IndexMask &mask = constraint.point_evaluator->get_evaluated_selection_as_mask();
    if (mask.is_empty()) {
      continue;
    }
    const SimPointsKey &mesh_key = keys[constraint.mesh_key_i];

    const int mesh_bundle_i = world_bundles.geometries.index_of_as(mesh_key.path);
    const Mesh &original_mesh = *world_bundles.geometries[mesh_bundle_i].geometry.get_mesh();

    const Span<int3> corner_tris = original_mesh.corner_tris();
    const Span<int> corner_verts = original_mesh.corner_verts();

    const VArraySpan<float2> uv_map = constraint.mesh_evaluator->get_evaluated<float2>(
        constraint.uv_map_index);
    const geometry::ReverseUVSampler reverse_uv_sampler(uv_map, corner_tris);

    const VArray<float2> sample_uvs_varray = constraint.point_evaluator->get_evaluated<float2>(
        constraint.sample_uv_index);
    const VArray<float> compliance_terms_varray = constraint.point_evaluator->get_evaluated<float>(
        constraint.compliance_term_index);

    Vector<int> &indices = scope.construct<Vector<int>>();
    Vector<int3> &triangle_indices = scope.construct<Vector<int3>>();
    Vector<float3> &bary_weights = scope.construct<Vector<float3>>();
    Vector<float> &compliance_terms = scope.construct<Vector<float>>();
    mask.foreach_index([&](const int point_i) {
      const float2 sample_uv = sample_uvs_varray[point_i];
      const geometry::ReverseUVSampler::Result result = reverse_uv_sampler.sample(sample_uv);
      if (result.type != geometry::ReverseUVSampler::ResultType::Ok) {
        return;
      }
      indices.append(point_i);
      bary_weights.append(result.bary_weights);

      const int3 corners = corner_tris[result.tri_index];
      const int3 triangle_verts{
          corner_verts[corners[0]], corner_verts[corners[1]], corner_verts[corners[2]]};
      triangle_indices.append(triangle_verts);
      compliance_terms.append(compliance_terms_varray[point_i]);
    });

    if (indices.is_empty()) {
      continue;
    }

    result.append(&scope.construct<xpbd::AttachUVSurfaceConstraintSet>(constraint.mesh_key_i,
                                                                       constraint.points_key_i,
                                                                       indices,
                                                                       triangle_indices,
                                                                       bary_weights,
                                                                       compliance_terms));
  }
  return result;
}

PROFILE_FUNCTION static Vector<xpbd::DistanceConstraintSet *>
gather_distance_based_edge_bending_constraints(ResourceScope &scope,
                                               XPBDState &state,
                                               const WorldPreprocessData &world_info,
                                               const WorldBundles &world_bundles,
                                               const Span<GeometrySet> applied_geometries,
                                               const VectorSet<SimPointsKey> &keys)
{
  Vector<xpbd::DistanceConstraintSet *> result;
  for (const DistanceBasedEdgeBendingConstraintData &constraint :
       world_info.distance_based_edge_bending_constraints)
  {
    const IndexMask &mask = constraint.evaluator->get_evaluated_selection_as_mask();
    if (mask.is_empty()) {
      continue;
    }

    const int key_i = constraint.key_i;
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const GeometrySet &applied_geometry = applied_geometries[geometry_bundle_i];
    const Mesh &mesh = *applied_geometry.get_mesh();

    const Span<int2> edges = mesh.edges();
    const Span<float3> mesh_positions = mesh.vert_positions();
    const Span<int3> corners_tris = mesh.corner_tris();
    const Span<int> corners_verts = mesh.corner_verts();

    MultiValueMap<OrderedEdge, int> tris_by_edge;
    for (const int tri_i : corners_tris.index_range()) {
      const int3 &tri = corners_tris[tri_i];
      const int v0 = corners_verts[tri[0]];
      const int v1 = corners_verts[tri[1]];
      const int v2 = corners_verts[tri[2]];
      tris_by_edge.add(OrderedEdge{v0, v1}, tri_i);
      tris_by_edge.add(OrderedEdge{v1, v2}, tri_i);
      tris_by_edge.add(OrderedEdge{v2, v0}, tri_i);
    }

    const VArray<float> compliance_terms_varray = constraint.evaluator->get_evaluated<float>(
        constraint.compliance_term_index);
    Vector<int2> &point_pairs = scope.construct<Vector<int2>>();
    Vector<float> &compliance_terms = scope.construct<Vector<float>>();

    mask.foreach_index([&](const int edge_i) {
      const int2 &edge = edges[edge_i];
      const Span<int> tris_at_edge = tris_by_edge.lookup(OrderedEdge(edge));
      if (tris_at_edge.size() <= 1) {
        /* Bending constraint needs at least two triangles. */
        return;
      }
      const float compliance_term = compliance_terms_varray[edge_i];
      for (const int tri0 : tris_at_edge.index_range()) {
        for (const int tri1 : tris_at_edge.index_range().drop_front(tri0 + 1)) {
          const int3 &tri0_corners = corners_tris[tris_at_edge[tri0]];
          const int3 &tri1_corners = corners_tris[tris_at_edge[tri1]];
          const int point_i0 = edge[0] ^ edge[1] ^ corners_verts[tri0_corners[0]] ^
                               corners_verts[tri0_corners[1]] ^ corners_verts[tri0_corners[2]];
          const int point_i1 = edge[0] ^ edge[1] ^ corners_verts[tri1_corners[0]] ^
                               corners_verts[tri1_corners[1]] ^ corners_verts[tri1_corners[2]];
          point_pairs.append({point_i0, point_i1});
          compliance_terms.append(compliance_term);
        }
      }
    });

    const Span<float> constraint_lengths = prepare_distance_constraint_lengths(
        scope, state, mesh_positions, key, point_pairs);

    result.append(&scope.construct<xpbd::DistanceConstraintSet>(
        key_i, point_pairs, constraint_lengths, compliance_terms));
  }
  return result;
}

struct StaticPlaneContacts {
  Vector<int> indices;
  Vector<float3> plane_positions;
  Vector<float3> plane_normals;
  Vector<float> static_frictions;
  Vector<float> dynamic_frictions;
  Vector<float> depths;
  Vector<float> compliance_terms;
};

struct DynamicSphereContacts {
  Vector<int2> indices;
  Vector<float> min_distance;
};

struct Contacts {
  Map<SimPointsKey, StaticPlaneContacts> static_plane_contacts;
  Map<SimPointsKey, DynamicSphereContacts> dynamic_sphere_contacts;
};

PROFILE_FUNCTION static void gather_ground_plane_contacts(
    const SimPoints &sim_points,
    const IndexRange points_range,
    const InfinitePlaneColliderData &collider,
    const Span<float> sim_points_frictions,
    const Span<float> sim_points_inverse_masses,
    StaticPlaneContacts &r_contacts)
{
  const float3 plane_normal = math::normalize(collider.normal);
  if (math::is_zero(plane_normal)) {
    return;
  }
  for (const int point_i : points_range) {
    const float inverse_mass = sim_points_inverse_masses[point_i];
    if (math::is_zero(inverse_mass)) {
      /* Points with infinite mass are pinned and don't collide dynamically. */
      continue;
    }

    const float3 &position = sim_points.positions[point_i];
    const float distance = math::dot(position - collider.position, plane_normal);
    if (distance >= 0.0f) {
      continue;
    }
    r_contacts.indices.append(point_i);
    r_contacts.plane_positions.append(collider.position);
    r_contacts.plane_normals.append(plane_normal);
    const float point_friction = sim_points_frictions[point_i];
    const float friction = math::sqrt(point_friction * collider.friction);
    r_contacts.static_frictions.append(friction);
    r_contacts.dynamic_frictions.append(friction);
    r_contacts.depths.append(-distance);
    r_contacts.compliance_terms.append(0.0f);
  }
}

PROFILE_FUNCTION static void gather_mesh_contacts(const XPBDState &state,
                                                  const SimPoints &sim_points,
                                                  const IndexRange points_range,
                                                  const ExternelMeshColliderData &collider,
                                                  const Span<float> sim_points_frictions,
                                                  const Span<float> sim_points_inverse_masses,
                                                  const float substep_factor,
                                                  const float delta_time,
                                                  StaticPlaneContacts &r_contacts)
{
  const ExternalColliderState *collider_state = state.external_colliders.lookup_ptr(
      collider.collider_key);
  const float compliance_term_factor = compute_compliance_factor(delta_time);

  float4x4 mesh_transform = collider.transform;
  if (collider_state) {
    mesh_transform = math::interpolate(
        collider_state->transform, collider.transform, substep_factor);
  }
  const float4x4 mesh_transform_inv = math::invert(mesh_transform);

  bke::BVHTreeFromMesh bvh = collider.mesh->bvh_corner_tris();
  for (const int point_i : points_range) {
    const float inverse_mass = sim_points_inverse_masses[point_i];
    if (inverse_mass <= 0.0f) {
      /* Points with infinite mass are pinned and don't collide dynamically. */
      continue;
    }

    const float3 &position_self = sim_points.positions[point_i];
    const float3 &position_mesh = math::transform_point(mesh_transform_inv, position_self);

    BVHTreeNearest nearest{};
    nearest.dist_sq = FLT_MAX;
    BLI_bvhtree_find_nearest(bvh.tree, position_mesh, &nearest, bvh.nearest_callback, &bvh);
    if (nearest.index == -1) {
      continue;
    }
    const float3 dir = float3(nearest.co) - position_mesh;
    const bool is_inside = math::dot(dir, float3(nearest.no)) > 0.0f;
    if (!is_inside) {
      continue;
    }

    const float penetration = math::length(dir);
    const float point_friction = sim_points_frictions[point_i];
    const float friction = math::sqrt(point_friction * collider.friction);

    const float3 collision_point = math::transform_point(mesh_transform, float3(nearest.co));
    const float3 collision_normal = math::transpose(float3x3(mesh_transform_inv)) *
                                    float3(nearest.no);

    r_contacts.indices.append(point_i);
    r_contacts.plane_positions.append(collision_point);
    r_contacts.plane_normals.append(collision_normal);
    r_contacts.static_frictions.append(friction);
    r_contacts.dynamic_frictions.append(friction);
    r_contacts.depths.append(penetration);
    r_contacts.compliance_terms.append(
        std::max(0.0f, compliance_term_factor * collider.compliance));
  }
}

PROFILE_FUNCTION static void gather_sphere_contacts(const SimPoints &sim_points,
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

PROFILE_FUNCTION static Contacts gather_contacts_curve_local(
    const int key_i,
    const IndexRange curves_range,
    const OffsetIndices<int> points_by_curve,
    const XPBDState &state,
    const WorldPreprocessData &world_info,
    const Span<SimPointsKey> keys,
    const SimPointsWorldProperties &props,
    const float substep_factor,
    const float delta_time)
{
  Contacts contacts;
  const SimPointsKey &key = keys[key_i];
  const SimPoints &sim_points = state.sim_points.lookup(key);
  const IndexRange points_range = points_by_curve[curves_range];

  StaticPlaneContacts plane_contacts;
  for (const InfinitePlaneColliderData &collider : world_info.infinite_plane_colliders) {
    if (collider.key_i != key_i) {
      continue;
    }
    gather_ground_plane_contacts(
        sim_points, points_range, collider, props.frictions, props.inverse_masses, plane_contacts);
  }
  for (const ExternelMeshColliderData &collider : world_info.mesh_colliders) {
    if (collider.key_i != key_i) {
      continue;
    }
    gather_mesh_contacts(state,
                         sim_points,
                         points_range,
                         collider,
                         props.frictions,
                         props.inverse_masses,
                         substep_factor,
                         delta_time,
                         plane_contacts);
  }
  if (!plane_contacts.indices.is_empty()) {
    contacts.static_plane_contacts.add_new(key, std::move(plane_contacts));
  }
  return contacts;
}

PROFILE_FUNCTION static Contacts gather_contacts_global(
    const Span<int> key_group,
    const XPBDState &state,
    const WorldPreprocessData &world_info,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries,
    const Span<SimPointsKey> keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const float substep_factor,
    const float delta_time)
{
  Contacts contacts;
  for (const int key_i : key_group) {
    const SimPointsKey &key = keys[key_i];
    const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
    const bke::GeometryComponent::Type type = key.type;
    const bke::GeometryComponent *component = applied_geometries[geometry_bundle_i].get_component(
        type);
    const SimPoints &sim_points = state.sim_points.lookup(key);
    if (!component) {
      continue;
    }
    const SimPointsWorldProperties &props = sim_points_props.lookup(key);

    {
      StaticPlaneContacts plane_contacts;
      for (const InfinitePlaneColliderData &collider : world_info.infinite_plane_colliders) {
        if (collider.key_i != key_i) {
          continue;
        }
        gather_ground_plane_contacts(sim_points,
                                     IndexRange(sim_points.points_num),
                                     collider,
                                     props.frictions,
                                     props.inverse_masses,
                                     plane_contacts);
      }
      for (const ExternelMeshColliderData &collider : world_info.mesh_colliders) {
        if (collider.key_i != key_i) {
          continue;
        }
        gather_mesh_contacts(state,
                             sim_points,
                             IndexRange(sim_points.points_num),
                             collider,
                             props.frictions,
                             props.inverse_masses,
                             substep_factor,
                             delta_time,
                             plane_contacts);
      }
      if (!plane_contacts.indices.is_empty()) {
        contacts.static_plane_contacts.add_new(key, std::move(plane_contacts));
      }
    }

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
      for (const SphericalSelfCollisionData &constraint : world_info.spherical_self_collisions) {
        if (constraint.key_i != key_i) {
          continue;
        }
        gather_sphere_contacts(sim_points, radii_span, sphere_contacts);
      }
      if (!sphere_contacts.indices.is_empty()) {
        contacts.dynamic_sphere_contacts.add_new(key, std::move(sphere_contacts));
      }
    }
  }
  return contacts;
}

PROFILE_FUNCTION static void generate_collision_constraint_sets(
    ResourceScope &scope,
    const Contacts &contacts,
    const VectorSet<SimPointsKey> &keys,
    const float delta_time,
    xpbd::ConstraintSetCollector &r_constraints)
{
  for (auto item : contacts.static_plane_contacts.items()) {
    const int key_i = keys.index_of(item.key);
    const StaticPlaneContacts &plane_contacts = item.value;
    r_constraints.general.append(
        &scope.construct<xpbd::CollisionPlaneConstraintSet>(key_i,
                                                            plane_contacts.indices,
                                                            plane_contacts.plane_positions,
                                                            plane_contacts.plane_normals,
                                                            plane_contacts.compliance_terms));
  }
  for (auto item : contacts.dynamic_sphere_contacts.items()) {
    const int key_i = keys.index_of(item.key);
    const DynamicSphereContacts &sphere_contacts = item.value;
    const float compliance_term = math::safe_divide(1e-4f, pow2f(delta_time));
    r_constraints.general.append(&scope.construct<xpbd::MinimumDistanceConstraintSet>(
        key_i,
        sphere_contacts.indices,
        sphere_contacts.min_distance,
        scope.allocator().construct_array<float>(sphere_contacts.indices.size(),
                                                 compliance_term)));
  }
}

PROFILE_FUNCTION static Array<GeometrySet> gather_applied_geometries(
    const XPBDState &state, const WorldBundles &world_bundles)
{
  Array<GeometrySet> applied_geometries(world_bundles.geometries.size());
  const int points_num = state.total_points_num();
  threading::memory_bandwidth_bound_task(points_num * sizeof(float3), [&]() {
    for (const int bundle_i : world_bundles.geometries.index_range()) {
      const XPBDGeometryBundle &geometry_bundle = world_bundles.geometries[bundle_i];
      GeometrySet &applied_geometry = applied_geometries[bundle_i];
      applied_geometry = apply_simulation(geometry_bundle, state);
    }
  });
  return applied_geometries;
}

PROFILE_FUNCTION static void update_sim_points_from_world(
    XPBDState &state,
    const WorldBundles &world_bundles,
    const Span<GeometrySet> applied_geometries)
{
  Map<SimPointsKey, SimPoints> new_sim_points;
  for (const int bundle_i : world_bundles.geometries.index_range()) {
    const GeometrySet &applied_geometry = applied_geometries[bundle_i];
    update_xpbd_state_for_geometry(
        state, world_bundles.geometries[bundle_i], applied_geometry, new_sim_points);
  }
  state.sim_points = std::move(new_sim_points);
}

PROFILE_FUNCTION static void reset_state_usages(XPBDState &state)
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

PROFILE_FUNCTION static void remove_unused_states(XPBDState &state)
{
  for (DistanceConstraintLengths &distance_constraint_lengths :
       state.distance_constraint_lengths.values())
  {
    distance_constraint_lengths.lengths.remove_if(
        [](const auto &item) { return !item.value.used; });
  }
}

PROFILE_FUNCTION static void solve_constraints(const SolverType solver_type,
                                               const Span<xpbd::GeometryRef> geometry_refs,
                                               const Span<xpbd::ConstraintSet *> constraint_sets)
{
  switch (solver_type) {
    case SolverType::SerialGaussSeidel: {
      xpbd::solve_gauss_seidel_one_at_a_time(geometry_refs, constraint_sets);
      break;
    }
    case SolverType::ParallelGaussSeidel: {
      xpbd::solve_gauss_seidel_parallel(geometry_refs, constraint_sets);
      break;
    }
    case SolverType::NonDeterministicJacobian: {
      xpbd::solve_jacobian_non_deterministic(geometry_refs, constraint_sets);
      break;
    }
  }
}

PROFILE_FUNCTION static void apply_static_plane_contact_friction(
    const StaticPlaneContacts &plane_contacts,
    const Span<float3> prev_positions,
    const int prev_positions_offset,
    const MutableSpan<float3> new_positions)
{
  for (const int contact_i : plane_contacts.indices.index_range()) {
    const int point_i = plane_contacts.indices[contact_i];
    const float3 &plane_normal = plane_contacts.plane_normals[contact_i];
    const float static_friction = plane_contacts.static_frictions[contact_i];
    const float dynamic_friction = plane_contacts.dynamic_frictions[contact_i];
    const float depth = plane_contacts.depths[contact_i];
    const float3 pos_diff = new_positions[point_i] -
                            prev_positions[point_i - prev_positions_offset];
    const float3 tangential_pos_diff = pos_diff - plane_normal * math::dot(pos_diff, plane_normal);
    float3 offset = tangential_pos_diff;
    const float tangential_dist = math::length(tangential_pos_diff);
    if (tangential_dist >= static_friction * depth) {
      offset *= std::min(dynamic_friction * depth / tangential_dist, 1.0f);
    }
    new_positions[point_i] -= offset;
  }
}

PROFILE_FUNCTION static void apply_friction(const Span<int> key_group,
                                            const Contacts &contacts,
                                            const VectorSet<SimPointsKey> &keys,
                                            const Span<Array<float3>> all_prev_positions,
                                            const Span<xpbd::GeometryRef> geometry_refs)
{
  for (const auto item : contacts.static_plane_contacts.items()) {
    const int key_i = keys.index_of(item.key);
    const int key_in_group_i = key_group.first_index(key_i);
    const Span<float3> prev_positions = all_prev_positions[key_in_group_i];
    apply_static_plane_contact_friction(
        item.value, prev_positions, 0, geometry_refs[key_i].positions);
  }
}

PROFILE_FUNCTION static void update_linear_velocities(const float delta_time,
                                                      const IndexRange range,
                                                      const Span<float3> prev_positions,
                                                      const Span<float3> new_positions,
                                                      MutableSpan<float3> r_velocities)
{
  const float inv_delta_time = math::safe_rcp(delta_time);
  for (const int i : range.index_range()) {
    const float3 &prev_position = prev_positions[i];
    const float3 &new_position = new_positions[i];
    const float3 diff = new_position - prev_position;
    const float3 velocity = diff * inv_delta_time;
    r_velocities[i] = velocity;
  }
}

PROFILE_FUNCTION static void update_angular_velocities(const float delta_time,
                                                       const IndexRange range,
                                                       const Span<math::Quaternion> prev_rotations,
                                                       const Span<math::Quaternion> new_rotations,
                                                       MutableSpan<float3> r_angular_velocities)
{
  const float inv_delta_time = math::safe_rcp(delta_time);
  for (const int i : range.index_range()) {
    float3 diff = (math::invert_normalized(prev_rotations[i]) * new_rotations[i]).imaginary_part();
    for (const int j : IndexRange(3)) {
      if (math::abs(diff[j]) < 1e-5f) {
        diff[j] = 0.0f;
      }
    }
    const float3 new_angular_velocity = 2.0f * diff * inv_delta_time;
    r_angular_velocities[i] = new_angular_velocity;
  }
}

PROFILE_FUNCTION static Vector<xpbd::GeometryRef> prepare_geometry_refs_for_solver(
    XPBDState &state,
    const Span<SimPointsKey> keys,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props)
{
  Vector<xpbd::GeometryRef> geometry_refs;
  for (const SimPointsKey &key : keys) {
    SimPoints &sim_points = state.sim_points.lookup(key);
    const SimPointsWorldProperties &props = sim_points_props.lookup(key);
    xpbd::GeometryRef geometry_ref;
    geometry_ref.positions = sim_points.positions;
    geometry_ref.inverse_masses = props.inverse_masses;
    if (sim_points.has_rotation) {
      geometry_ref.rotations = sim_points.rotations;
      geometry_ref.inertias = props.inertias;
      geometry_ref.inverse_inertias = props.inverse_inertias;
    }
    geometry_refs.append(geometry_ref);
  }
  return geometry_refs;
}

PROFILE_FUNCTION static Map<SimPointsKey, PinnedPositions> compute_pinned_positions(
    const XPBDState &state,
    const WorldPreprocessData &world_info,
    const VectorSet<SimPointsKey> &keys)
{
  Map<SimPointsKey, PinnedPositions> result;
  for (const PinnedPositionConstraintData &constraint : world_info.pinned_position_constraints) {
    const int key_i = constraint.key_i;
    const SimPointsKey &key = keys[key_i];
    const SimPoints &sim_points = state.sim_points.lookup(key);

    PinnedPositions &pinned_positions = result.lookup_or_add_default(key);
    const IndexMask &mask = constraint.evaluator->get_evaluated_selection_as_mask();
    const int mask_size = mask.size();
    const VArray<float3> positions = constraint.evaluator->get_evaluated<float3>(
        constraint.position_index);
    const VArray<float> compliance_terms = constraint.evaluator->get_evaluated<float>(
        constraint.compliance_terms_index);

    if (const std::optional<float> compliance_term = compliance_terms.get_if_single()) {
      if (compliance_term == 0.0f) {
        const int old_size = pinned_positions.hard_indices.size();
        pinned_positions.hard_indices.resize(old_size + mask_size);
        pinned_positions.hard_animations.resize(old_size + mask_size);
        mask.foreach_index(GrainSize(512), [&](const int point_i, const int pos) {
          const float3 &new_position = positions[point_i];
          const float3 &old_position = state.is_initialization() ? new_position :
                                                                   sim_points.positions[point_i];
          pinned_positions.hard_indices[old_size + pos] = point_i;
          pinned_positions.hard_animations[old_size + pos] = {old_position, new_position};
        });
      }
      else {
        const int old_size = pinned_positions.soft_indices.size();
        pinned_positions.soft_indices.resize(old_size + mask_size);
        pinned_positions.soft_animations.resize(old_size + mask_size);
        pinned_positions.soft_compliance_terms.resize(old_size + mask_size);
        mask.foreach_index(GrainSize(512), [&](const int point_i, const int pos) {
          const float3 &new_position = positions[point_i];
          const float3 &old_position = state.is_initialization() ? new_position :
                                                                   sim_points.positions[point_i];
          pinned_positions.soft_indices[old_size + pos] = point_i;
          pinned_positions.soft_compliance_terms[old_size + pos] = compliance_terms[point_i];
          pinned_positions.soft_animations[old_size + pos] = {old_position, new_position};
        });
      }
    }
    else {
      mask.foreach_index([&](const int point_i) {
        const float3 &new_position = positions[point_i];
        const float3 &old_position = state.is_initialization() ? new_position :
                                                                 sim_points.positions[point_i];
        const float compliance_term = compliance_terms[point_i];
        StartStopPair<float3> animation{old_position, new_position};
        if (compliance_term == 0.0f) {
          pinned_positions.hard_indices.append(point_i);
          pinned_positions.hard_animations.append(animation);
        }
        else {
          pinned_positions.soft_indices.append(point_i);
          pinned_positions.soft_compliance_terms.append(compliance_term);
          pinned_positions.soft_animations.append(animation);
        }
      });
    }
  }
  return result;
}

PROFILE_FUNCTION static Map<SimPointsKey, PinnedRotations> compute_pinned_rotations(
    const XPBDState &state,
    const WorldPreprocessData &world_info,
    const VectorSet<SimPointsKey> &keys)
{
  Map<SimPointsKey, PinnedRotations> result;
  for (const PinnedRotationConstraintData &constraint : world_info.pinned_rotation_constraints) {
    const int key_i = constraint.key_i;
    const SimPointsKey &key = keys[key_i];
    const SimPoints &sim_points = state.sim_points.lookup(key);

    PinnedRotations &pinned_rotations = result.lookup_or_add_default(key);
    const IndexMask &mask = constraint.evaluator->get_evaluated_selection_as_mask();
    const int mask_size = mask.size();
    const VArray<math::Quaternion> &rotations =
        constraint.evaluator->get_evaluated<math::Quaternion>(constraint.rotation_index);
    const VArray<float> &compliance_terms = constraint.evaluator->get_evaluated<float>(
        constraint.compliance_terms_index);

    if (const std::optional<float> compliance_term = compliance_terms.get_if_single()) {
      if (compliance_term == 0.0f) {
        const int old_size = pinned_rotations.hard_indices.size();
        pinned_rotations.hard_indices.resize(old_size + mask_size);
        pinned_rotations.hard_animations.resize(old_size + mask_size);
        mask.foreach_index(GrainSize(512), [&](const int point_i, const int pos) {
          const math::Quaternion &new_rotation = rotations[point_i];
          const math::Quaternion &old_rotation = state.is_initialization() ?
                                                     new_rotation :
                                                     sim_points.rotations[point_i];
          pinned_rotations.hard_indices[old_size + pos] = point_i;
          pinned_rotations.hard_animations[old_size + pos] = {old_rotation, new_rotation};
        });
      }
      else {
        const int old_size = pinned_rotations.soft_indices.size();
        pinned_rotations.soft_indices.resize(old_size + mask_size);
        pinned_rotations.soft_animations.resize(old_size + mask_size);
        pinned_rotations.soft_compliance_terms.resize(old_size + mask_size);
        mask.foreach_index(GrainSize(512), [&](const int point_i, const int pos) {
          const math::Quaternion &new_rotation = rotations[point_i];
          const math::Quaternion &old_rotation = state.is_initialization() ?
                                                     new_rotation :
                                                     sim_points.rotations[point_i];
          pinned_rotations.soft_indices[old_size + pos] = point_i;
          pinned_rotations.soft_compliance_terms[old_size + pos] = compliance_terms[point_i];
          pinned_rotations.soft_animations[old_size + pos] = {old_rotation, new_rotation};
        });
      }
    }
    else {
      mask.foreach_index([&](const int point_i) {
        const math::Quaternion &new_rotation = rotations[point_i];
        const math::Quaternion &old_rotation = state.is_initialization() ?
                                                   new_rotation :
                                                   sim_points.rotations[point_i];
        const float compliance_term = compliance_terms[point_i];
        StartStopPair<math::Quaternion> animation{old_rotation, new_rotation};
        if (compliance_term == 0.0f) {
          pinned_rotations.hard_indices.append(point_i);
          pinned_rotations.hard_animations.append(animation);
        }
        else {
          pinned_rotations.soft_indices.append(point_i);
          pinned_rotations.soft_compliance_terms.append(compliance_term);
          pinned_rotations.soft_animations.append(animation);
        }
      });
    }
  }
  return result;
}

template<typename T>
static IndexRange find_indices_in_range(const Span<T> indices, const IndexRange range)
{
  if (range.is_empty()) {
    return {};
  }
  const int64_t start = binary_search::first_if(
      indices.begin(), indices.end(), [&](const int index) { return index >= range.start(); });
  const int64_t last = binary_search::last_if(
      indices.begin(), indices.end(), [&](const int index) {
        return index < range.one_after_last();
      });
  return IndexRange::from_begin_end_inclusive(start, last);
}

PROFILE_FUNCTION static void update_pinned_positions(const IndexRange range,
                                                     const PinnedPositions &pinned_positions,
                                                     const float factor,
                                                     MutableSpan<float3> r_positions,
                                                     MutableSpan<float3> r_soft_pinned_positions)
{
  for (const int i : find_indices_in_range<int>(pinned_positions.hard_indices, range)) {
    const int point_i = pinned_positions.hard_indices[i];
    const float3 current_position = pinned_positions.hard_animations[i].interpolate(factor);
    r_positions[point_i - range.start()] = current_position;
  }
  for (const int i : find_indices_in_range<int>(pinned_positions.soft_indices, range)) {
    const float3 current_position = pinned_positions.soft_animations[i].interpolate(factor);
    r_soft_pinned_positions[i] = current_position;
  }
}

PROFILE_FUNCTION static void updated_pinned_rotations(
    const IndexRange range,
    const PinnedRotations &pinned_rotations,
    const float factor,
    MutableSpan<math::Quaternion> r_rotations,
    MutableSpan<math::Quaternion> r_soft_pinned_rotations)
{
  for (const int i : find_indices_in_range<int>(pinned_rotations.hard_indices, range)) {
    const int point_i = pinned_rotations.hard_indices[i];
    const math::Quaternion current_rotation = pinned_rotations.hard_animations[i].interpolate(
        factor);
    r_rotations[point_i - range.start()] = current_rotation;
  }
  for (const int i : find_indices_in_range<int>(pinned_rotations.soft_indices, range)) {
    const math::Quaternion current_rotation = pinned_rotations.soft_animations[i].interpolate(
        factor);
    r_soft_pinned_rotations[i] = current_rotation;
  }
}

static void pre_solve_per_point_steps(const IndexRange range,
                                      const MutableSpan<float3> prev_positions,
                                      const MutableSpan<math::Quaternion> prev_rotations,
                                      const MutableSpan<float3> positions,
                                      const MutableSpan<float3> velocities,
                                      const MutableSpan<math::Quaternion> rotations,
                                      const MutableSpan<float3> angular_velocities,
                                      const SimPointsWorldProperties &props,
                                      const Span<float3> accelerations,
                                      const std::optional<Span<float3>> torques,
                                      const PinnedPositions *pinned_positions,
                                      const PinnedRotations *pinned_rotations,
                                      const float substep_factor,
                                      MutableSpan<float3> soft_pinned_positions,
                                      MutableSpan<math::Quaternion> soft_pinned_rotations,
                                      const float delta_time)
{
  prev_positions.copy_from(positions);
  prev_rotations.copy_from(rotations);
  if (delta_time > 0.0f) {
    integrate_linear_velocities(
        delta_time, range, prev_positions, accelerations, props, positions, velocities);
    if (!rotations.is_empty()) {
      integrate_angular_velocities(
          delta_time, range, torques, props, prev_rotations, rotations, angular_velocities);
    }
  }
  if (pinned_positions) {
    update_pinned_positions(
        range, *pinned_positions, substep_factor, positions, soft_pinned_positions);
  }
  if (pinned_rotations) {
    updated_pinned_rotations(
        range, *pinned_rotations, substep_factor, rotations, soft_pinned_rotations);
  }
}

static void post_solve_per_point_steps(const float delta_time,
                                       const IndexRange range,
                                       const Span<float3> prev_positions,
                                       const Span<math::Quaternion> prev_rotations,
                                       const Span<float3> new_positions,
                                       const Span<math::Quaternion> new_rotations,
                                       MutableSpan<float3> r_velocities,
                                       MutableSpan<float3> r_angular_velocities)
{
  if (delta_time > 0.0f) {
    update_linear_velocities(delta_time, range, prev_positions, new_positions, r_velocities);
    if (!new_rotations.is_empty()) {
      update_angular_velocities(
          delta_time, range, prev_rotations, new_rotations, r_angular_velocities);
    }
  }
}

PROFILE_FUNCTION static void simulate_key_group_global(
    const Span<int> key_group,
    ThreadLocalStorage &tls,
    XPBDState &state,
    const WorldBundles &world_bundles,
    const WorldPreprocessData &world_info,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, Span<float3>> &accelerations_map,
    const Map<SimPointsKey, Span<float3>> &torques_map,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const Map<SimPointsKey, PinnedPositions> &pinned_positions_map,
    const Map<SimPointsKey, PinnedRotations> &pinned_rotations_map,
    const Map<SimPointsKey, MutableSpan<float3>> &soft_pinned_positions_map,
    const Map<SimPointsKey, MutableSpan<math::Quaternion>> &soft_pinned_rotations_map,
    const Span<GeometrySet> applied_geometries,
    const SolverType solver_type,
    const Span<xpbd::GeometryRef> geometry_refs,
    const xpbd::ConstraintSetCollector &filtered_static_constraint_sets,
    const int substeps,
    const float sub_delta_time)
{
  ResourceScope &scope = tls.local_resource_scope();
  const int keys_in_group_num = key_group.size();

  Array<Array<float3>> all_prev_positions(keys_in_group_num);
  Array<Array<math::Quaternion>> all_prev_rotations(keys_in_group_num);
  for (const int key_in_group_i : key_group.index_range()) {
    const int key_i = key_group[key_in_group_i];
    const SimPointsKey &key = keys[key_i];
    const SimPoints &sim_points = state.sim_points.lookup(key);
    all_prev_positions[key_in_group_i].reinitialize(sim_points.points_num);
    if (sim_points.has_rotation) {
      all_prev_rotations[key_in_group_i].reinitialize(sim_points.points_num);
    }
  }

  /* Instead of doing various stages like remembering old positions and updating velocities one
   * after another, interleave them to improve cache locality and thread utilization. This is
   * possible because each point is processed independently here. */
  auto run_per_point_updates = [&](const float substep_factor,
                                   const bool do_pre_solve,
                                   const bool do_post_solve) {
    threading::parallel_for(IndexRange(keys_in_group_num), 1, [&](const IndexRange range) {
      for (const int key_in_group_i : range) {
        const int key_i = key_group[key_in_group_i];
        const SimPointsKey &key = keys[key_i];
        SimPoints &sim_points = state.sim_points.lookup(keys[key_i]);
        const Span<float3> accelerations = accelerations_map.lookup(key);
        const std::optional<Span<float3>> torques = torques_map.lookup_try(key);
        const SimPointsWorldProperties &props = sim_points_props.lookup(key);
        const PinnedPositions *pinned_positions = pinned_positions_map.lookup_ptr(key);
        const PinnedRotations *pinned_rotations = pinned_rotations_map.lookup_ptr(key);
        MutableSpan<float3> soft_pinned_positions =
            soft_pinned_positions_map.lookup_try(key).value_or(MutableSpan<float3>({}));
        MutableSpan<math::Quaternion> soft_pinned_rotations =
            soft_pinned_rotations_map.lookup_try(key).value_or(MutableSpan<math::Quaternion>({}));
        threading::parallel_for(
            IndexRange(sim_points.points_num), 256, [&](const IndexRange range) {
              /* The post-solve steps are run first here, because this code runs at the end of the
               * time-step after the constraints are solved. */
              if (do_post_solve) {
                post_solve_per_point_steps(
                    sub_delta_time,
                    range,
                    all_prev_positions[key_in_group_i].as_span().slice(range),
                    all_prev_rotations[key_in_group_i].as_span().slice_safe(range),
                    sim_points.positions.as_span().slice(range),
                    sim_points.rotations.as_mutable_span().slice_safe(range),
                    sim_points.velocities.as_mutable_span().slice(range),
                    sim_points.angular_velocities.as_mutable_span().slice_safe(range));
              }
              if (do_pre_solve) {
                pre_solve_per_point_steps(
                    range,
                    all_prev_positions[key_in_group_i].as_mutable_span().slice(range),
                    all_prev_rotations[key_in_group_i].as_mutable_span().slice_safe(range),
                    sim_points.positions.as_mutable_span().slice(range),
                    sim_points.velocities.as_mutable_span().slice(range),
                    sim_points.rotations.as_mutable_span().slice_safe(range),
                    sim_points.angular_velocities.as_mutable_span().slice_safe(range),
                    props,
                    accelerations,
                    torques,
                    pinned_positions,
                    pinned_rotations,
                    substep_factor,
                    soft_pinned_positions,
                    soft_pinned_rotations,
                    sub_delta_time);
              }
            });
      }
    });
  };

  for (const int substep_i : IndexRange(substeps)) {
    const float substep_factor = substeps <= 1 ? 1.0f : float(substep_i) / (substeps - 1);
    const bool is_first_substep = substep_i == 0;
    const bool is_last_substep = substep_i == substeps - 1;

    /* In all other substeps, this is done at the end of the previous step already to improve
     * parallelism and cache locality. */
    if (is_first_substep) {
      run_per_point_updates(substep_factor, true, false);
    }

    /* Find current collisions and generate constraints to resolve them. */
    const Contacts contacts = gather_contacts_global(key_group,
                                                     state,
                                                     world_info,
                                                     world_bundles,
                                                     applied_geometries,
                                                     keys,
                                                     sim_points_props,
                                                     substep_factor,
                                                     sub_delta_time);
    xpbd::ConstraintSetCollector dynamic_constraint_sets;
    generate_collision_constraint_sets(
        scope, contacts, keys, sub_delta_time, dynamic_constraint_sets);

    /* Combine static and dynamic constraint sets. */
    const Vector<xpbd::ConstraintSet *> current_constraint_sets =
        xpbd::ConstraintSetCollector::combine(
            scope, {&filtered_static_constraint_sets, &dynamic_constraint_sets});

    /* Actually solve the constraints. */
    solve_constraints(solver_type, geometry_refs, current_constraint_sets);

    if (sub_delta_time > 0.0f) {
      /* Apply friction by updating current positions before the new velocity is computed. */
      apply_friction(key_group, contacts, keys, all_prev_positions, geometry_refs);
    }

    /* Does remaining per-point updates at the end of this time step (like updating velocities) and
     * also does the beginning of the next timestep already unless this is the last substep. */
    run_per_point_updates(substep_factor, !is_last_substep, true);
  }
}

static xpbd::SolveStrategyType get_solve_strategy_type(const SolverType solver_type)
{
  switch (solver_type) {
    case SolverType::SerialGaussSeidel:
      return xpbd::SolveStrategyType::GaussSeidelOneAtATime;
    case SolverType::ParallelGaussSeidel:
      return xpbd::SolveStrategyType::GaussSeidelParallel;
    case SolverType::NonDeterministicJacobian:
      return xpbd::SolveStrategyType::JacobianNonDeterministic;
  }
  return xpbd::SolveStrategyType::GaussSeidelParallel;
}

PROFILE_FUNCTION static void simulate_curve_local(
    const int key_i,
    ThreadLocalStorage &tls,
    XPBDState &state,
    const WorldBundles &world_bundles,
    const WorldPreprocessData &world_info,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, Span<float3>> &accelerations_map,
    const Map<SimPointsKey, Span<float3>> &torques_map,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const Map<SimPointsKey, PinnedPositions> &pinned_positions_map,
    const Map<SimPointsKey, PinnedRotations> &pinned_rotations_map,
    const Map<SimPointsKey, MutableSpan<float3>> &soft_pinned_positions_map,
    const Map<SimPointsKey, MutableSpan<math::Quaternion>> &soft_pinned_rotations_map,
    const Span<GeometrySet> applied_geometries,
    const SolverType solver_type,
    const Span<xpbd::GeometryRef> geometry_refs,
    const xpbd::ConstraintSetCollector &filtered_static_constraint_sets,
    const int substeps,
    const float sub_delta_time)
{
  const SimPointsKey &key = keys[key_i];
  SimPoints &sim_points = state.sim_points.lookup(key);
  const int geometry_bundle_i = world_bundles.geometries.index_of_as(key.path);
  const Curves &curves_id = *applied_geometries[geometry_bundle_i].get_curves();
  const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();

  const Span<float3> accelerations = accelerations_map.lookup(key);
  const std::optional<Span<float3>> torques = torques_map.lookup_try(key);
  const SimPointsWorldProperties &props = sim_points_props.lookup(key);
  const PinnedPositions *pinned_positions = pinned_positions_map.lookup_ptr(key);
  const PinnedRotations *pinned_rotations = pinned_rotations_map.lookup_ptr(key);
  MutableSpan<float3> soft_pinned_positions = soft_pinned_positions_map.lookup_try(key).value_or(
      MutableSpan<float3>({}));
  MutableSpan<math::Quaternion> soft_pinned_rotations =
      soft_pinned_rotations_map.lookup_try(key).value_or(MutableSpan<math::Quaternion>({}));

  threading::parallel_for(
      curves.curves_range(),
      256,
      [&](const IndexRange curves_range) {
        ResourceScope &scope = tls.local_resource_scope();
        const IndexRange points_range = points_by_curve[curves_range];
        const int points_num = points_range.size();
        Array<float3, 1024> prev_positions(points_num);
        Array<math::Quaternion, 1024> prev_rotations(sim_points.has_rotation ? points_num : 0);
        xpbd::ConstraintSetParams params{geometry_refs};
        for ([[maybe_unused]] const int substep_i : IndexRange(substeps)) {
          const float substep_factor = substeps <= 1 ? 1.0f : float(substep_i) / (substeps - 1);
          pre_solve_per_point_steps(
              points_range,
              prev_positions,
              prev_rotations,
              sim_points.positions.as_mutable_span().slice(points_range),
              sim_points.velocities.as_mutable_span().slice(points_range),
              sim_points.rotations.as_mutable_span().slice_safe(points_range),
              sim_points.angular_velocities.as_mutable_span().slice_safe(points_range),
              props,
              accelerations,
              torques,
              pinned_positions,
              pinned_rotations,
              substep_factor,
              soft_pinned_positions,
              soft_pinned_rotations,
              sub_delta_time);

          const Contacts contacts = gather_contacts_curve_local(key_i,
                                                                curves_range,
                                                                points_by_curve,
                                                                state,
                                                                world_info,
                                                                keys,
                                                                props,
                                                                substep_factor,
                                                                sub_delta_time);
          xpbd::ConstraintSetCollector dynamic_constraint_sets;
          generate_collision_constraint_sets(
              scope, contacts, keys, sub_delta_time, dynamic_constraint_sets);

          xpbd::SolveStrategy solve_strategy{
              get_solve_strategy_type(solver_type), geometry_refs, key_i, points_range};
          for (xpbd::CurveLocalConstraintSet *constraint_set :
               filtered_static_constraint_sets.curve_local)
          {
            constraint_set->solve_step(solve_strategy, params, curves_range);
          }
          for (xpbd::ConstraintSet *constraint_set : dynamic_constraint_sets.general) {
            constraint_set->solve_step(solve_strategy, params);
          }
          solve_strategy.apply();

          if (sub_delta_time > 0.0f) {
            /* Apply friction by updating current positions before the new velocity is computed.*/
            if (const StaticPlaneContacts *plane_contacts =
                    contacts.static_plane_contacts.lookup_ptr(key))
            {
              apply_static_plane_contact_friction(
                  *plane_contacts, prev_positions, points_range.start(), sim_points.positions);
            }
          }

          post_solve_per_point_steps(
              sub_delta_time,
              points_range,
              prev_positions,
              prev_rotations,
              sim_points.positions.as_span().slice(points_range),
              sim_points.rotations.as_span().slice_safe(points_range),
              sim_points.velocities.as_mutable_span().slice(points_range),
              sim_points.angular_velocities.as_mutable_span().slice_safe(points_range));
        }
      },
      threading::accumulated_task_sizes(
          [&](const IndexRange curves_range) { return points_by_curve[curves_range].size(); }));
}

static bool supports_curve_local_evaluation(const Span<int> key_group,
                                            const VectorSet<SimPointsKey> &keys,
                                            const WorldPreprocessData &world_info,
                                            const xpbd::ConstraintSetCollector &constraint_sets)
{
  if (key_group.size() != 1) {
    /* If the group is larger, it means that there are interactions between multiple geometries, so
     * it can't be curve-local. */
    return false;
  }
  const int key_i = key_group[0];
  const SimPointsKey &key = keys[key_i];
  if (key.type != bke::GeometryComponent::Type::Curve) {
    return false;
  }
  if (!constraint_sets.general.is_empty()) {
    /* General constraints are not curve-local. */
    return false;
  }
  for (const SphericalSelfCollisionData &constraint : world_info.spherical_self_collisions) {
    if (constraint.key_i == key_i) {
      /* Spherical self collisions are not curve-local. */
      return false;
    }
  }
  return true;
}

PROFILE_FUNCTION static void simulate_key_group(
    const Span<int> key_group,
    ThreadLocalStorage &tls,
    XPBDState &state,
    const WorldBundles &world_bundles,
    const WorldPreprocessData &world_info,
    const VectorSet<SimPointsKey> &keys,
    const Map<SimPointsKey, Span<float3>> &accelerations_map,
    const Map<SimPointsKey, Span<float3>> &torques_map,
    const Map<SimPointsKey, SimPointsWorldProperties> &sim_points_props,
    const Map<SimPointsKey, PinnedPositions> &pinned_positions_map,
    const Map<SimPointsKey, PinnedRotations> &pinned_rotations_map,
    const Map<SimPointsKey, MutableSpan<float3>> &soft_pinned_positions_map,
    const Map<SimPointsKey, MutableSpan<math::Quaternion>> &soft_pinned_rotations_map,
    const Span<GeometrySet> applied_geometries,
    const SolverType solver_type,
    const Span<xpbd::GeometryRef> geometry_refs,
    const xpbd::ConstraintSetCollector &static_constraint_sets,
    const int substeps,
    const float sub_delta_time)
{
  xpbd::ConstraintSetCollector filtered_constraint_sets;
  for (xpbd::ConstraintSet *constraint_set : static_constraint_sets.general) {
    const Span<int> affected_keys = constraint_set->get_affected_geo_indices();
    if (key_group.contains(affected_keys[0])) {
      filtered_constraint_sets.general.append(constraint_set);
    }
  }
  for (xpbd::CurveLocalConstraintSet *constraint_set : static_constraint_sets.curve_local) {
    if (key_group.contains(constraint_set->affected_geo_i())) {
      filtered_constraint_sets.curve_local.append(constraint_set);
    }
  }

  if (supports_curve_local_evaluation(key_group, keys, world_info, filtered_constraint_sets)) {
    simulate_curve_local(key_group[0],
                         tls,
                         state,
                         world_bundles,
                         world_info,
                         keys,
                         accelerations_map,
                         torques_map,
                         sim_points_props,
                         pinned_positions_map,
                         pinned_rotations_map,
                         soft_pinned_positions_map,
                         soft_pinned_rotations_map,
                         applied_geometries,
                         solver_type,
                         geometry_refs,
                         filtered_constraint_sets,
                         substeps,
                         sub_delta_time);
  }
  else {
    simulate_key_group_global(key_group,
                              tls,
                              state,
                              world_bundles,
                              world_info,
                              keys,
                              accelerations_map,
                              torques_map,
                              sim_points_props,
                              pinned_positions_map,
                              pinned_rotations_map,
                              soft_pinned_positions_map,
                              soft_pinned_rotations_map,
                              applied_geometries,
                              solver_type,
                              geometry_refs,
                              filtered_constraint_sets,
                              substeps,
                              sub_delta_time);
  }
}

PROFILE_FUNCTION static void update_and_step_xpbd_state(XPBDState &state,
                                                        const WorldBundles &world_bundles,
                                                        const float total_delta_time,
                                                        const SolverType solver_type,
                                                        const int substeps)
{
  ThreadLocalStorage tls;
  ResourceScope &scope = tls.local_resource_scope();
  const float sub_delta_time = math::safe_divide<float>(total_delta_time, substeps);

  const Array<GeometrySet> applied_geometries = gather_applied_geometries(state, world_bundles);
  update_sim_points_from_world(state, world_bundles, applied_geometries);

  VectorSet<SimPointsKey> keys;
  for (const SimPointsKey &key : state.sim_points.keys()) {
    keys.add_new(key);
  }

  reset_state_usages(state);

  WorldPreprocessData world_info;
  prepare_evaluation__forces(scope, world_info, world_bundles, applied_geometries, keys);
  prepare_evaluation__torques(scope, world_info, world_bundles, state, applied_geometries, keys);
  prepare_evaluation__curves_rod_stretch_and_shear_constraints(
      scope, world_info, state, world_bundles, applied_geometries, keys, sub_delta_time);
  prepare_evaluation__curves_rod_bend_and_twist_constraints(
      scope, world_info, state, world_bundles, applied_geometries, keys, sub_delta_time);
  prepare_evaluation__pinned_position_constraints(
      scope, world_info, world_bundles, applied_geometries, keys, sub_delta_time);
  prepare_evaluation__pinned_rotation_constraints(
      scope, world_info, state, world_bundles, applied_geometries, keys, sub_delta_time);
  prepare_evaluation__sim_points_props(
      scope, world_info, world_bundles, state, applied_geometries, keys);
  prepare_evaluation__edge_length_constraints(
      scope, world_info, world_bundles, applied_geometries, keys, sub_delta_time);
  prepare_evaluation__curve_length_constraints(
      scope, world_info, world_bundles, applied_geometries, keys, sub_delta_time);
  prepare_evaluation__align_positions_constraints(
      scope, world_info, world_bundles, applied_geometries, keys);
  prepare_evaluation__attach_uv_surface_constraints(
      scope, world_info, world_bundles, applied_geometries, keys, sub_delta_time);
  prepare_evaluation__distance_based_edge_bending_constraints(
      scope, world_info, world_bundles, applied_geometries, keys, sub_delta_time);
  prepare_evaluation__pressure_constraints(world_info, world_bundles, applied_geometries, keys);
  prepare_evaluation__infinite_plane_colliders(world_info, world_bundles, keys);
  prepare_evaluation__spherical_self_collisions(world_info, world_bundles, keys);
  prepare_evaluation__colliders(world_info, world_bundles, keys);

  Vector<fn::FieldEvaluator *> field_evaluators;
  for (fn::FieldEvaluator *evaluator : world_info.field_evaluators.values()) {
    field_evaluators.append(evaluator);
  }

  threading::parallel_for(
      field_evaluators.index_range(),
      1024,
      [&](const IndexRange range) {
        for (fn::FieldEvaluator *evaluator : field_evaluators.as_span().slice(range)) {
          evaluator->evaluate();
        }
      },
      threading::individual_task_sizes(
          [&](const int i) { return field_evaluators[i]->evaluation_mask().size(); }));

  Map<SimPointsKey, PinnedPositions> pinned_positions_map = compute_pinned_positions(
      state, world_info, keys);
  Map<SimPointsKey, PinnedRotations> pinned_rotations_map = compute_pinned_rotations(
      state, world_info, keys);

  const Map<SimPointsKey, SimPointsWorldProperties> sim_points_props =
      compute_sim_point_world_properties(world_bundles,
                                         world_info,
                                         keys,
                                         applied_geometries,
                                         pinned_positions_map,
                                         pinned_rotations_map);
  const Map<SimPointsKey, Span<float3>> accelerations_map = compute_external_accelerations(
      tls, world_bundles, world_info, state, keys, sim_points_props);
  const Map<SimPointsKey, Span<float3>> torques_map = compute_external_torques(
      tls, state, world_info, keys);

  xpbd::ConstraintSetCollector static_constraint_sets;

  for (xpbd::CurveLocalConstraintSet *constraint_set :
       build_constraint_sets__curves_rod_stretch_and_shear(
           tls, world_info, state, world_bundles, applied_geometries, keys))
  {
    static_constraint_sets.curve_local.append(constraint_set);
  }
  for (xpbd::CurveLocalConstraintSet *constraint_set :
       build_constraint_sets__curves_rod_bend_and_twist(
           tls, world_info, state, world_bundles, applied_geometries, keys))
  {
    static_constraint_sets.curve_local.append(constraint_set);
  }
  for (xpbd::DistanceConstraintSet *constraint_set : gather_edge_length_constraints(
           scope, state, world_info, world_bundles, applied_geometries, keys))
  {
    static_constraint_sets.general.append(constraint_set);
  }
  for (xpbd::PressureConstraintSet *constraint_set : gather_pressure_constraints(
           scope, state, world_info, world_bundles, applied_geometries, keys))
  {
    static_constraint_sets.general.append(constraint_set);
  }
  for (xpbd::DistanceConstraintSet *constraint_set : gather_curve_segment_constraints(
           scope, state, world_info, world_bundles, applied_geometries, keys))
  {
    static_constraint_sets.general.append(constraint_set);
  }
  for (xpbd::AlignPositionsConstraintSet *constraint_set :
       gather_align_positions_constraints(scope, world_info, sub_delta_time))
  {
    static_constraint_sets.general.append(constraint_set);
  }
  for (xpbd::AttachUVSurfaceConstraintSet *constraint_set :
       gather_attach_uv_surface_constraints(scope, world_info, world_bundles, keys))
  {
    static_constraint_sets.general.append(constraint_set);
  }
  for (xpbd::DistanceConstraintSet *constraint_set :
       gather_distance_based_edge_bending_constraints(
           scope, state, world_info, world_bundles, applied_geometries, keys))
  {
    static_constraint_sets.general.append(constraint_set);
  }
  const Map<SimPointsKey, MutableSpan<float3>> soft_pinned_positions_map =
      gather_soft_pinned_position_constraints(
          scope, keys, pinned_positions_map, static_constraint_sets);
  const Map<SimPointsKey, MutableSpan<math::Quaternion>> soft_pinned_rotations_map =
      gather_soft_pinned_rotation_constraints(
          scope, keys, pinned_rotations_map, static_constraint_sets);

  remove_unused_states(state);

  const Vector<xpbd::GeometryRef> geometry_refs = prepare_geometry_refs_for_solver(
      state, keys, sim_points_props);

  DisjointSet<int> disjoint_set(keys.size());
  for (const xpbd::ConstraintSet *constraint_set : static_constraint_sets.general) {
    const Span<int> affected_keys = constraint_set->get_affected_geo_indices();
    for (const int i : affected_keys.index_range().drop_back(1)) {
      disjoint_set.join(affected_keys[i], affected_keys[i + 1]);
    }
  }
  MultiValueMap<int, int> independent_key_groups_map;
  for (const int key_i : keys.index_range()) {
    const int group_id = disjoint_set.find_root(key_i);
    independent_key_groups_map.add(group_id, key_i);
  }
  Vector<Span<int>> independent_key_groups;
  independent_key_groups.extend(independent_key_groups_map.values().begin(),
                                independent_key_groups_map.values().end());

  threading::parallel_for(
      independent_key_groups.index_range(), 1, [&](const IndexRange key_group_range) {
        for (const int key_group_i : key_group_range) {
          const Span<int> key_group = independent_key_groups[key_group_i];
          simulate_key_group(key_group,
                             tls,
                             state,
                             world_bundles,
                             world_info,
                             keys,
                             accelerations_map,
                             torques_map,
                             sim_points_props,
                             pinned_positions_map,
                             pinned_rotations_map,
                             soft_pinned_positions_map,
                             soft_pinned_rotations_map,
                             applied_geometries,
                             solver_type,
                             geometry_refs,
                             static_constraint_sets,
                             substeps,
                             sub_delta_time);
        }
      });

  state.external_colliders.clear();
  for (const ExternelMeshColliderData &collider : world_info.mesh_colliders) {
    state.external_colliders.add(collider.collider_key, ExternalColliderState{collider.transform});
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

  WorldBundles world_bundles = parse_world(*world_bundle_ptr);
  XPBDState &state = xpbd_state_owner->state;

  const bool is_resimulating = update_counter < state.update_counter;
  update_counter++;
  if (!is_resimulating) {
    update_and_step_xpbd_state(state, world_bundles, delta_time, solver_type, substeps);
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

  Array<GeometrySet> applied_geometries = gather_applied_geometries(state, world_bundles);
  for (const int bundle_i : world_bundles.geometries.index_range()) {
    const XPBDGeometryBundle &bundle = world_bundles.geometries[bundle_i];
    GeometrySet &applied_geometry = applied_geometries[bundle_i];
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
