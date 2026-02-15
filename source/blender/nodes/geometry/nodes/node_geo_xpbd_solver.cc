/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_bvhutils.hh"
#include "BKE_instances.hh"
#include "BLI_stack.hh"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"

#include "BKE_curves.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_grease_pencil.hh"

#include "GEO_xpbd.hh"
#include "GEO_xpbd_constraint_sets_common.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"
#include "NOD_geometry_nodes_physics_bundles.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_xpbd_solver_cc {

using namespace physics_bundles;

namespace attribute_names {
constexpr StringRefNull position = "position";
constexpr StringRefNull velocity = "velocity";
constexpr StringRefNull rotation = "rotation";
constexpr StringRefNull angular_velocity = "angular_velocity";
constexpr StringRefNull external_force = "external_force";
constexpr StringRefNull external_torque = "external_torque";
constexpr StringRefNull mass = "mass";
constexpr StringRefNull moment_of_inertia = "moment_of_inertia";
constexpr StringRefNull friction = "friction";
constexpr StringRefNull rest_length = "rest_length";
constexpr StringRefNull rest_bend_rotation = "rest_bend_rotation";

/** True for pinned points. */
constexpr StringRefNull sim_pin_position = "sim_pin_position";
/** Begin and end pin position for the current time step. The position is interpolated. */
constexpr StringRefNull sim_pin_position_begin = "sim_pin_position_begin";
constexpr StringRefNull sim_pin_position_end = "sim_pin_position_end";
/** Compliance of the pin position constraint. */
constexpr StringRefNull sim_pin_position_compliance = "sim_pin_position_compliance";
constexpr StringRefNull sim_pin_position_lambda = "sim_pin_position_lambda";

constexpr StringRefNull sim_pin_rotation = "sim_pin_rotation";
constexpr StringRefNull sim_pin_rotation_begin = "sim_pin_rotation_begin";
constexpr StringRefNull sim_pin_rotation_end = "sim_pin_rotation_end";
constexpr StringRefNull sim_pin_rotation_compliance = "sim_pin_rotation_compliance";
constexpr StringRefNull sim_pin_rotation_lambda = "sim_pin_rotation_lambda";

}  // namespace attribute_names

static NestedBundleTypePtr make_world_type()
{
  Vector<std::shared_ptr<const FlatBundleType>> types;
  types.append(GravityBundle::get_bundle_type());
  types.append(XPBDGeometryBundle::get_bundle_type());
  types.append(DampingBundle::get_bundle_type());
  types.append(InfiniteGroundPlaneBundle::get_bundle_type());
  types.append(ColliderBundle::get_bundle_type());
  types.append(RodStretchAndShearXPBDConstraintBundle::get_bundle_type());
  types.append(RodBendAndTwistXPBDConstraintBundle::get_bundle_type());

  NestedBundleTypePtr world_type = std::make_shared<const NestedBundleType>("Blender.XpbdWorld",
                                                                            std::move(types));
  BundleTypeRegistry::register_type(world_type);
  return world_type;
}

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

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  static NestedBundleTypePtr world_type = make_world_type();
  b.add_input<decl::Bundle>("World")
      .bundle_type(world_type)
      .field_on_all()
      .structure_type(StructureType::Single)
      .description("World state that is updated by the solver");
  b.add_output<decl::Bundle>("World").pass_through_input_index(0).align_with_previous();
  b.add_input<decl::Float>("Delta Time")
      .min(0)
      .default_value(1 / 25.0f)
      .subtype(PROP_TIME_ABSOLUTE);

  auto &panel = b.add_panel("Solver").default_closed(true);
  panel.add_input<decl::Menu>("Solver Type")
      .static_items(solver_type_items)
      .default_value(SolverType::ParallelGaussSeidel);
  panel.add_input<decl::Int>("Substeps").default_value(10).min(1);
  panel.add_input<decl::Int>("Constraint Iterations").default_value(1).min(1);
}

struct DataKey {
  int geo_bundle_i;
  bke::GeometryComponent::Type type;
  std::optional<int> layer_i;

  uint64_t hash() const
  {
    return get_default_hash(this->geo_bundle_i, this->type, this->layer_i.value_or(0));
  }

  friend bool operator==(const DataKey &a, const DataKey &b) = default;
};

struct GeometryDataChunk {
  int data_key_i;
  IndexRange points_range;
  /** Only used when the geometry data has curves. */
  std::optional<IndexRange> curves_range;
};

struct DampingConstraint {
  std::string path;
  Field<float> linear_damping;
  Field<float> angular_damping;
};
struct DampingConstraintUsage {
  /** Index of corresponding #DampingConstraint. */
  int constraint_i;
  VArray<float> linear_dampings;
  VArray<float> angular_dampings;
  MutableSpan<float> linear_damping_lambdas;
  MutableSpan<float> angular_damping_lambdas;
};

struct RodStretchShearConstraint {
  std::string path;
  Field<float> compliance;
};
struct RodStretchShearConstraintUsage {
  /** Index of corresponding #RodStretchShearConstraint. */
  int constraint_i;
  VArray<float> compliances;
  MutableSpan<float3> lambdas_pos;
  MutableSpan<float3> lambdas_rot;
};

struct InfinitePlaneCollider {
  std::string path;
  float3 end_position;
  float3 end_normal;
  float3 begin_position;
  float3 begin_normal;
  float friction;
};
struct InfinitePlaneColliderUsage {
  /** Index of corresponding #InfinitePlaneCollider. */
  int constraint_i;
};

struct MeshCollider {
  std::string path;
  Vector<int> instance_ids;
  const Mesh *mesh;
  bke::BVHTreeFromMesh corner_tris_bvh;
  float4x4 begin_transform;
  float4x4 end_transform;
  float friction;
  float compliance;
};
struct MeshColliderUsage {
  /** Index of corresponding #MeshCollider. */
  int constraint_i;
};

struct GeometryData {
  bke::MutableAttributeAccessor attributes;
  AttrDomain domain;
  int size;
  /** Easy access to curve data for curves and grease pencil layers. */
  bke::CurvesGeometry *curves = nullptr;
  Vector<std::string> tags;

  IndexRange chunks;
  /**
   * Allows fast lookup of which chunk a point belongs to. This allows e.g. grouping collisions by
   * chunk efficiently.
   */
  Array<int> point_to_chunk;

  bke::SpanAttributeWriter<float3> position_attr;
  bke::SpanAttributeWriter<float3> velocity_attr;
  bke::SpanAttributeWriter<math::Quaternion> rotation_attr;
  bke::SpanAttributeWriter<float3> angular_velocity_attr;
  VArraySpan<float3> external_force_attr;
  VArraySpan<float3> external_torque_attr;

  VArraySpan<float> frictions;

  /** Indexed by point index. */
  VArraySpan<float3> pin_position_begin;
  VArraySpan<float3> pin_position_end;
  bke::SpanAttributeWriter<float> pin_position_lambda_attr;
  /** Indexed by pin index. */
  Array<float> pin_position_lambdas;
  /** The current target position, this is updated in each substep. Indexed by pin index.*/
  Array<float3> pin_position_current;

  /* Indexed by point index. */
  VArraySpan<math::Quaternion> pin_rotation_begin;
  VArraySpan<math::Quaternion> pin_rotation_end;
  /** Note, these are not really quaternions, but there is no float4 attribute type yet. */
  bke::SpanAttributeWriter<math::Quaternion> pin_rotation_lambda_attr;
  /** Indexed by pin index. */
  Array<float4> pin_rotation_lambdas;
  Array<math::Quaternion> pin_rotation_current;

  /* Only a single constraint of this type is allowed. */
  bool has_rod_bend_twist_constraint = false;
  VArray<float> rod_bend_twist_compliances;
  MutableSpan<float4> rod_bend_twist_lambdas;

  VArraySpan<float> rest_lengths;
  VArraySpan<math::Quaternion> rest_bend_rotations;

  /**
   * Temporary arrays for positions and rotations. This is necessary because xpbd requires the old
   * and new positions in the end to compute the new velocities.
   */
  Array<float3> temp_positions;
  Array<math::Quaternion> temp_rotations;

  /**
   * Inverse of the mass attribute + extra changes:
   * - For hard pinned positions this is set to 0. // TODO: actually support hard pinning
   * - If there is no mass attribute, or it is <= 0, this is set to 1.
   */
  Array<float> inv_masses;
  Array<float3> inv_moments_of_inertia;
  /**
   * This does not match the inertia attribute exactly, since it has special handling for special
   * cases like pinning (for which it is infinity).
   */
  Array<float3> moments_of_inertia;

  Vector<InfinitePlaneColliderUsage> infinite_plane_colliders;
  Vector<DampingConstraintUsage> damping_constraints;
  Vector<MeshColliderUsage> mesh_colliders;

  /** Only a single constraint of this type is allowed. */
  std::optional<RodStretchShearConstraintUsage> rod_stretch_shear_constraint;
};

struct GeometrySetData {
  std::string path;
  /**
   * The geometry is moved out of the world bundle for local processing and is moved back in the
   * end.
   */
  GeometrySet geometry;
  VectorSet<std::string> tags;
};

struct Geometries {
  Vector<GeometrySetData> geometry_sets;

  VectorSet<DataKey> data_keys;
  Vector<GeometryData> data;

  /**
   * By having static chunks, across the entire solve step allows for more efficient
   * multi-threading (and preparation for multi-threading).
   */
  Vector<GeometryDataChunk> chunks;
  int max_chunk_size = -1;

  /**
   * Two arrays of geometry references are used because the arrays containing the previous and
   * current positions/rotations are swapped for different time steps to avoid unnecessary copying.
   *
   * Index 0: Previous data is stored in temporary arrays, current data in the geometry attributes.
   * Index 1: Previous data is stored in the geometry attributes, current data in temporary arrays.
   */
  std::array<Array<xpbd::GeometryRef>, 2> solver_refs;
};

struct FieldEvaluatorKey {
  int data_key_i;
  AttrDomain domain;
  Field<bool> selection;

  friend bool operator==(const FieldEvaluatorKey &a, const FieldEvaluatorKey &b) = default;

  uint64_t hash() const
  {
    return get_default_hash(this->data_key_i, this->domain, this->selection ? this->selection : 0);
  }
};

static const Field<bool> &get_constant_true_field()
{
  static const Field<bool> field = fn::make_constant_field<bool>(true);
  return field;
}

struct SubstepInterval {
  float begin_factor;
  float end_factor;
  bool is_first;
  bool is_last;

  SubstepInterval(const int substeps, const int current_i)
      : begin_factor(float(current_i) / substeps),
        end_factor(float(current_i + 1) / substeps),
        is_first(current_i == 0),
        is_last(current_i == substeps - 1)
  {
  }
};

struct InfinitePlaneContactId {
  int infinite_plane_collider_i;
  int point_i;

  uint64_t hash() const
  {
    return get_default_hash(this->infinite_plane_collider_i, this->point_i);
  }

  friend bool operator==(const InfinitePlaneContactId &a,
                         const InfinitePlaneContactId &b) = default;
};

struct MeshContactId {
  int mesh_collider_i;
  int point_i;

  uint64_t hash() const
  {
    return get_default_hash(this->mesh_collider_i, this->point_i);
  }

  friend bool operator==(const MeshContactId &a, const MeshContactId &b) = default;
};

struct ExternalPlaneContacts {
  Map<MeshContactId, int> mesh_contact_indices;
  Map<InfinitePlaneContactId, int> infinite_plane_contact_indices;

  Vector<int> points;
  Vector<float3> positions_on_plane;
  /* The movement of the collider in the current substep. */
  Vector<float3> collider_motion;
  Vector<float3> collider_velocities;
  Vector<float3> separating_axes;
  Vector<float> static_frictions;
  Vector<float> dynamic_frictions;
  Vector<float> compliance_terms;

  Vector<bool> active_states;
  Vector<float> lambdas_normal;
  Vector<float> lambdas;

  void init_or_preserve_state(const ExternalPlaneContacts &prev_contacts,
                              const std::optional<int> &prev_i)
  {
    if (prev_i) {
      this->active_states.append(prev_contacts.active_states[*prev_i]);
      this->lambdas_normal.append(prev_contacts.lambdas_normal[*prev_i]);
      this->lambdas.append(prev_contacts.lambdas[*prev_i]);
    }
    else {
      this->active_states.append(false);
      this->lambdas_normal.append(0.0f);
      this->lambdas.append(0.0f);
    }
  }
};

struct ChunkConstraints {
  Vector<xpbd::ConstraintSet *> static_constraints;
  Vector<xpbd::VelocityConstraintSet *> static_velocity_constraints;

  Span<int> pin_position_indices;
  MutableSpan<float3> pin_positions;
  Span<float> pin_position_lambdas;

  Span<int> pin_rotation_indices;
  MutableSpan<math::Quaternion> pin_rotations;
  Span<float4> pin_rotation_lambdas;

  ExternalPlaneContacts external_plane_contacts;
};

template<typename T> class VArraySpanGetter {
 private:
  const int max_range_size_;
  std::optional<Span<T>> full_span_;
  std::optional<Span<T>> chunk_span_;

 public:
  VArraySpanGetter(ResourceScope &scope, const VArray<T> &varray, const int max_range_size)
      : max_range_size_(max_range_size)
  {
    if (varray.is_span()) {
      full_span_ = varray.get_internal_span();
    }
    else if (const std::optional<T> single_value = varray.get_if_single()) {
      chunk_span_ = scope.allocator().construct_array<T>(max_range_size, *single_value);
    }
    else {
      MutableSpan<T> full_span = scope.allocator().allocate_array<T>(max_range_size);
      varray.materialize_to_uninitialized(full_span);
      full_span_ = full_span;
    }
  }

  Span<T> get_span_for_range(const IndexRange range) const
  {
    const int range_size = range.size();
    BLI_assert(range_size <= max_range_size_);
    if (full_span_) {
      return full_span_->slice(range);
    }
    return chunk_span_->take_front(range_size);
  }
};

struct ConstraintsInfo {
  Array<ChunkConstraints> chunk_constraints;
  Vector<InfinitePlaneCollider> infinite_plane_colliders;
  Vector<MeshCollider> mesh_colliders;
  Vector<DampingConstraint> damping_constraints;
  Vector<RodStretchShearConstraint> rod_stretch_shear_constraints;
};

class XpbdSolverStep {
 private:
  ResourceScope &global_scope_;
  LinearAllocator<> &global_allocator_;
  threading::EnumerableThreadSpecific<ResourceScope> thread_scopes_;
  IndexMaskMemory memory_;
  Bundle &world_;
  const int substeps_;
  const float sub_delta_time_;
  SolverType solver_type_;
  int constraint_iterations_;

  float substep_compliance_factor_;
  fn::Field<float> substep_compliance_factor_field_;

  Map<FieldEvaluatorKey, fn::FieldEvaluator *> field_evaluators_;

  Geometries geometries_;
  ConstraintsInfo constraints_info_;

 public:
  XpbdSolverStep(ResourceScope &scope,
                 Bundle &world,
                 const float total_delta_time,
                 const int substeps,
                 const SolverType solver_type,
                 const int constraint_iterations)
      : global_scope_(scope),
        global_allocator_(scope.allocator()),
        world_(world),
        substeps_(substeps),
        sub_delta_time_(total_delta_time / substeps_),
        solver_type_(solver_type),
        constraint_iterations_(constraint_iterations)
  {
  }

  void do_step()
  {
    this->prepare_substep_compliance_factor();
    this->gather_from_world__geometries();
    this->prepare_geometry_chunks();
    this->gather_from_world__infinite_plane_colliders();
    this->gather_from_world__mesh_colliders();
    this->gather_from_world__stretch_shear_constraints();
    this->gather_from_world__bend_twist_constraints();
    this->gather_from_world__damping();
    this->prepare_pinned_positions();
    this->prepare_pinned_rotations();
    this->prepare_inverse_masses();
    this->prepare_inverse_moments_of_inertia();

    this->evaluate_constraint_fields();

    this->create_constraints__rod_stretch_shear();
    this->create_constraints__rod_bend_twist();
    this->create_constraints__damping();

    this->do_simulation();
    this->finish_attribute_writers();
    this->write_back_geometries_to_world();
  }

 private:
  void prepare_substep_compliance_factor()
  {
    substep_compliance_factor_ = math::safe_rcp(pow2f(sub_delta_time_));
    substep_compliance_factor_field_ = fn::make_constant_field(substep_compliance_factor_);
  }

  void gather_from_world__geometries()
  {
    /* Gather geometry bundle paths. */
    const Vector<std::string> paths = gather_bundle_paths_by_type(world_,
                                                                  XPBDGeometryBundle::name);
    const int num_geometry_sets = paths.size();
    geometries_.geometry_sets.reinitialize(num_geometry_sets);

    for (const int i : IndexRange(num_geometry_sets)) {
      const StringRef path = paths[i];
      GeometrySetData &geo_set_data = geometries_.geometry_sets[i];
      geo_set_data.path = path;
      BundlePtr *geo_bundle_ptr = world_.lookup_path_for_write_ptr<BundlePtr>(path);
      if (!geo_bundle_ptr || !*geo_bundle_ptr) {
        continue;
      }
      Bundle &geo_bundle = geo_bundle_ptr->ensure_mutable_inplace();
      GeometrySet *geometry = geo_bundle.lookup_ptr<GeometrySet>("geometry");
      if (!geometry) {
        continue;
      }
      if (!geometry->has_bundle()) {
        continue;
      }
      if (GeometrySet *geometry = geo_bundle.lookup_path_for_write_ptr<GeometrySet>("geometry")) {
        geo_set_data.geometry = std::move(*geometry);
      }
      const Bundle &bundle_in_geo = *geo_set_data.geometry.bundle();
      if (const std::optional<ListPtr> tags_list_ptr = bundle_in_geo.lookup_path<ListPtr>("tags"))
      {
        if (*tags_list_ptr) {
          const List &tags_list = **tags_list_ptr;
          if (tags_list.cpp_type().is<std::string>()) {
            tags_list.foreach<std::string>(
                [&](const std::string &tag) { geo_set_data.tags.add(tag); });
          }
        }
      }
    }

    /* Gather individual components that should be simulated. There may be more than geometry sets
     * because each geometry set could contain e.g. a mesh and curves. */
    for (const int geo_bundle_i : IndexRange(num_geometry_sets)) {
      GeometrySetData &geo_set_data = geometries_.geometry_sets[geo_bundle_i];
      GeometrySet &geometry = geo_set_data.geometry;
      for (bke::GeometryComponent::Type type : {bke::GeometryComponent::Type::Mesh,
                                                bke::GeometryComponent::Type::PointCloud,
                                                bke::GeometryComponent::Type::Curve,
                                                bke::GeometryComponent::Type::Instance})
      {
        if (!geometry.has(type)) {
          continue;
        }
        bke::GeometryComponent &component = geometry.get_component_for_write(type);
        geometries_.data_keys.add_new({geo_bundle_i, type, std::nullopt});
        GeometryData geo_data{*component.attributes_for_write()};
        geo_data.curves = type == bke::GeometryComponent::Type::Curve ?
                              &geometry.get_curves_for_write()->geometry.wrap() :
                              nullptr;
        geo_data.domain = type == bke::GeometryComponent::Type::Instance ? AttrDomain::Instance :
                                                                           AttrDomain::Point;
        geometries_.data.append(std::move(geo_data));
      }
      if (geometry.has_grease_pencil()) {
        using namespace blender::bke::greasepencil;
        GreasePencil &grease_pencil = *geometry.get_grease_pencil_for_write();
        for (const int layer_i : grease_pencil.layers().index_range()) {
          Layer &layer = grease_pencil.layer(layer_i);
          Drawing *drawing = grease_pencil.get_eval_drawing(layer);
          if (!drawing) {
            continue;
          }
          bke::CurvesGeometry &curves = drawing->strokes_for_write();
          geometries_.data_keys.add_new(
              {geo_bundle_i, bke::GeometryComponent::Type::Curve, layer_i});
          GeometryData geo_data{curves.attributes_for_write()};
          geo_data.domain = AttrDomain::Point;
          geo_data.curves = &curves;
          geometries_.data.append(std::move(geo_data));
        }
      }
    }
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      const DataKey &data_key = geometries_.data_keys[data_key_i];
      GeometryData &geo_data = geometries_.data[data_key_i];
      const AttrDomain domain = geo_data.domain;
      geo_data.size = geo_data.attributes.domain_size(domain);
      geo_data.temp_positions.reinitialize(geo_data.size);
      geo_data.temp_rotations.reinitialize(geo_data.size);
      geo_data.position_attr = geo_data.attributes.lookup_or_add_for_write_span<float3>(
          attribute_names::position, domain);
      geo_data.velocity_attr = geo_data.attributes.lookup_or_add_for_write_span<float3>(
          attribute_names::velocity, domain);
      geo_data.rotation_attr = geo_data.attributes.lookup_or_add_for_write_span<math::Quaternion>(
          attribute_names::rotation, domain);
      geo_data.angular_velocity_attr = geo_data.attributes.lookup_or_add_for_write_span<float3>(
          attribute_names::angular_velocity, domain);
      geo_data.external_force_attr = *geo_data.attributes.lookup_or_default<float3>(
          attribute_names::external_force, domain, float3(0, 0, 0));
      geo_data.external_torque_attr = *geo_data.attributes.lookup_or_default<float3>(
          attribute_names::external_torque, domain, float3(0, 0, 0));
      geo_data.frictions = *geo_data.attributes.lookup_or_default<float>(
          attribute_names::friction, domain, 0.0f);
      if (geo_data.curves || data_key.type == bke::GeometryComponent::Type::Mesh) {
        geo_data.rest_lengths = *geo_data.attributes.lookup_or_default<float>(
            attribute_names::rest_length, geo_data.domain, 0.0f);
      }
      if (geo_data.curves) {
        geo_data.rest_bend_rotations = *geo_data.attributes.lookup_or_default<math::Quaternion>(
            attribute_names::rest_bend_rotation, geo_data.domain, math::Quaternion::identity());
      }
    }
  }

  void gather_from_world__infinite_plane_colliders()
  {
    const Vector<std::string> paths = gather_bundle_paths_by_type(world_,
                                                                  InfiniteGroundPlaneBundle::name);
    for (const StringRef path : paths) {
      const BundlePtr *bundle_ptr = world_.lookup_path_ptr<BundlePtr>(path);
      if (!bundle_ptr || !*bundle_ptr) {
        continue;
      }
      const Bundle &bundle = **bundle_ptr;
      const std::optional<float3> position = bundle.lookup<float3>("position");
      const std::optional<float3> normal = bundle.lookup<float3>("normal");
      const float friction = bundle.lookup<float>("friction").value_or(0.0f);
      if (!position || !normal) {
        continue;
      }
      if (math::is_zero(*normal)) {
        continue;
      }
      const float3 prev_position = bundle.lookup<float3>("prev_position").value_or(*position);
      float3 prev_normal = bundle.lookup<float3>("prev_normal").value_or(*normal);
      if (math::is_zero(prev_normal)) {
        prev_normal = *normal;
      }

      const int collider_i = constraints_info_.infinite_plane_colliders.append_and_get_index(
          {path,
           *position,
           math::normalize(*normal),
           prev_position,
           math::normalize(prev_normal),
           friction});
      for (const int data_key_i : geometries_.data_keys.index_range()) {
        if (this->behavior_applies_to_geometry(path, bundle, data_key_i)) {
          geometries_.data[data_key_i].infinite_plane_colliders.append({collider_i});
        }
      }
    }
  }

  bool behavior_applies_to_geometry(const StringRef behavior_path,
                                    const Bundle &behavior,
                                    const int data_key_i) const
  {
    const DataKey &data_key = geometries_.data_keys[data_key_i];
    const GeometrySetData &geo_set_data = geometries_.geometry_sets[data_key.geo_bundle_i];
    const StringRef geo_bundle_path = geo_set_data.path;

    const bool filter_local = behavior.lookup<bool>("filter_local").value_or(false);
    if (filter_local) {
      const int pos = behavior_path.rfind('/');
      if (pos == StringRef::not_found) {
        /* The behavior is at the root level, so a local filter applies to everything.*/
        return true;
      }
      const StringRef behavior_parent_path = behavior_path.substr(0, pos);
      if (geo_bundle_path.startswith(behavior_parent_path)) {
        return true;
      }
      return false;
    }
    const std::string filter = behavior.lookup<std::string>("filter").value_or("");
    if (filter.empty()) {
      return true;
    }
    StringRef remaining = filter;
    while (!remaining.is_empty()) {
      const int sep = remaining.find(',');
      if (sep == -1) {
        const StringRef tag = remaining.trim();
        if (geo_set_data.tags.contains_as(tag)) {
          return true;
        }
        return false;
      }
      const StringRef tag = remaining.substr(0, sep).trim();
      if (geo_set_data.tags.contains_as(tag)) {
        return true;
      }
      remaining = remaining.substr(sep + 1);
    }
    return false;
  }

  void gather_from_world__mesh_colliders()
  {
    const Vector<std::string> paths = gather_bundle_paths_by_type(world_, ColliderBundle::name);
    for (const StringRef path : paths) {
      const BundlePtr *bundle_ptr = world_.lookup_path_ptr<BundlePtr>(path);
      if (!bundle_ptr || !*bundle_ptr) {
        continue;
      }
      const Bundle &bundle = **bundle_ptr;
      const bke::GeometrySet *geometry = bundle.lookup_ptr<bke::GeometrySet>("geometry");
      const float friction = bundle.lookup<float>("friction").value_or(0.0f);
      const float compliance = bundle.lookup<float>("compliance").value_or(0.0f);
      const bke::GeometrySet *prev_geometry = bundle.lookup_ptr<bke::GeometrySet>("prev_geometry");
      if (!geometry) {
        continue;
      }
      Vector<int> affected_data;
      for (const int data_key_i : geometries_.data_keys.index_range()) {
        if (this->behavior_applies_to_geometry(path, bundle, data_key_i)) {
          affected_data.append(data_key_i);
        }
      }
      Vector<int> instance_id_stack;
      this->gather_colliders_in_geometry(path,
                                         float4x4::identity(),
                                         float4x4::identity(),
                                         *geometry,
                                         prev_geometry,
                                         friction,
                                         compliance,
                                         affected_data,
                                         instance_id_stack);
    }
  }

  void gather_colliders_in_geometry(const StringRef path,
                                    const float4x4 &transform,
                                    const float4x4 &prev_transform,
                                    const GeometrySet &collider_geo,
                                    const GeometrySet *prev_collider_geo,
                                    const float friction,
                                    const float compliance,
                                    const Span<int> affected_data,
                                    Vector<int> &instance_id_stack)
  {
    if (const Mesh *mesh = collider_geo.get_mesh()) {
      if (mesh->faces_num > 0) {
        const int collider_i = constraints_info_.mesh_colliders.append_and_get_index(
            {path,
             instance_id_stack,
             mesh,
             mesh->bvh_corner_tris(),
             prev_transform,
             transform,
             friction,
             compliance});
        for (const int data_key_i : affected_data) {
          geometries_.data[data_key_i].mesh_colliders.append({collider_i});
        }
      }
    }
    if (const bke::Instances *instances = collider_geo.get_instances()) {
      struct PrevInstanceItem {
        const float4x4 *transform;
        const bke::InstanceReference *reference;
      };
      Map<int, PrevInstanceItem> prev_instance_by_id;
      if (prev_collider_geo) {
        const bke::Instances *prev_instances = prev_collider_geo->get_instances();
        if (prev_instances) {
          const Span<float4x4> prev_instance_transforms = prev_instances->transforms();
          const Span<int> prev_instance_ids = prev_instances->unique_ids();
          const Span<bke::InstanceReference> prev_references = prev_instances->references();
          for (const int i : prev_instance_transforms.index_range()) {
            const int prev_instance_id = prev_instance_ids[i];
            const float4x4 &prev_instance_transform = prev_instance_transforms[i];
            const bke::InstanceReference &prev_reference = prev_references[i];
            prev_instance_by_id.add(prev_instance_id, {&prev_instance_transform, &prev_reference});
          }
        }
      }

      const Span<float4x4> instance_transforms = instances->transforms();
      const Span<bke::InstanceReference> references = instances->references();
      const Span<int> handles = instances->reference_handles();
      const Span<int> instance_ids = instances->unique_ids();
      for (const int instance_i : instance_transforms.index_range()) {
        const int handle = handles[instance_i];
        if (!references.index_range().contains(handle)) {
          continue;
        }
        const int instance_id = instance_ids[instance_i];
        const float4x4 instance_transform = instance_transforms[instance_i];
        const PrevInstanceItem *prev_instance_item = prev_instance_by_id.lookup_ptr(instance_id);
        const bke::InstanceReference &reference = references[handle];
        GeometrySet reference_geo;
        reference.to_geometry_set(reference_geo);
        GeometrySet prev_reference_geo;
        float4x4 prev_instance_transform = instance_transform;
        if (prev_instance_item) {
          prev_instance_item->reference->to_geometry_set(prev_reference_geo);
          prev_instance_transform = *prev_instance_item->transform;
        }

        instance_id_stack.append(instance_id);
        BLI_SCOPED_DEFER([&]() { instance_id_stack.pop_last(); });
        this->gather_colliders_in_geometry(path,
                                           transform * instance_transform,
                                           prev_transform * prev_instance_transform,
                                           reference_geo,
                                           prev_instance_item ? &prev_reference_geo : nullptr,
                                           friction,
                                           compliance,
                                           affected_data,
                                           instance_id_stack);
      }
    }
  }

  void prepare_geometry_chunks()
  {
    constexpr int approx_points_per_chunk = 256;
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      if (geo_data.size == 0) {
        continue;
      }
      const int old_chunks_num = geometries_.chunks.size();

      if (geo_data.curves) {
        /* For curve data, align the ranges with curve boundaries. */
        const bke::CurvesGeometry &curves = *geo_data.curves;
        const OffsetIndices<int> points_by_curve = curves.points_by_curve();
        Stack<IndexRange> curve_ranges_to_check;
        curve_ranges_to_check.push(curves.curves_range());
        while (!curve_ranges_to_check.is_empty()) {
          const IndexRange curves_range = curve_ranges_to_check.pop();
          const IndexRange points_range = points_by_curve[curves_range];
          if (points_range.is_empty()) {
            continue;
          }
          if (curves_range.size() == 1 || points_range.size() <= approx_points_per_chunk) {
            geometries_.chunks.append({data_key_i, points_range, curves_range});
            continue;
          }
          const int split_pos = curves_range.size() / 2;
          const IndexRange curves_range_left = curves_range.take_front(split_pos);
          const IndexRange curves_range_right = curves_range.drop_front(split_pos);
          /* Pushing in this order ensures that the chunks are sorted in the end. */
          curve_ranges_to_check.push(curves_range_right);
          curve_ranges_to_check.push(curves_range_left);
        }
      }
      else {
        const IndexRange points_range = IndexRange(geo_data.size);
        Stack<IndexRange> ranges_to_check;
        ranges_to_check.push(points_range);
        while (!ranges_to_check.is_empty()) {
          const IndexRange points_range = ranges_to_check.pop();
          if (points_range.is_empty()) {
            continue;
          }
          if (points_range.size() <= approx_points_per_chunk) {
            geometries_.chunks.append({data_key_i, points_range});
            continue;
          }
          const int split_pos = points_range.size() / 2;
          const IndexRange points_range_left = points_range.take_front(split_pos);
          const IndexRange points_range_right = points_range.drop_front(split_pos);
          /* Pushing in this order ensures that the chunks are sorted in the end. */
          ranges_to_check.push(points_range_right);
          ranges_to_check.push(points_range_left);
        }
      }
      geo_data.chunks = IndexRange::from_begin_end(old_chunks_num, geometries_.chunks.size());

      /* Create mapping from points to chunks. */
      geo_data.point_to_chunk.reinitialize(geo_data.size);
      for (const int chunk_i : geo_data.chunks) {
        const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
        geo_data.point_to_chunk.as_mutable_span().slice(chunk.points_range).fill(chunk_i);
      }
      constraints_info_.chunk_constraints.reinitialize(geometries_.chunks.size());
    }
    for (const int chunk_i : geometries_.chunks.index_range()) {
      const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
      geometries_.max_chunk_size = std::max<int>(geometries_.max_chunk_size,
                                                 chunk.points_range.size());
    }
  }

  void prepare_pinned_positions()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      const bke::AttributeReader<bool> pin_attr = geo_data.attributes.lookup<bool>(
          attribute_names::sim_pin_position, geo_data.domain);
      const bke::AttributeReader<float3> begin_attr = geo_data.attributes.lookup<float3>(
          attribute_names::sim_pin_position_begin, geo_data.domain);
      const bke::AttributeReader<float3> end_attr = geo_data.attributes.lookup<float3>(
          attribute_names::sim_pin_position_end, geo_data.domain);
      if (!pin_attr || !begin_attr || !end_attr) {
        continue;
      }

      const VArraySpan<bool> pin_attr_span = *pin_attr;
      geo_data.pin_position_begin = begin_attr.varray;
      geo_data.pin_position_end = end_attr.varray;
      geo_data.pin_position_lambda_attr = geo_data.attributes.lookup_or_add_for_write_span<float>(
          attribute_names::sim_pin_position_lambda,
          geo_data.domain,
          bke::AttributeInitValue(0.0f));

      const VArray<float> compliance_attr = *geo_data.attributes.lookup_or_default<float>(
          attribute_names::sim_pin_position_compliance, geo_data.domain, 0.0f);

      threading::parallel_for(
          geo_data.chunks.index_range(), 16, [&](const IndexRange chunk_range) {
            ResourceScope &thread_scope = thread_scopes_.local();
            LinearAllocator<> &thread_allocator = thread_scope.allocator();
            for (const int chunk_i : chunk_range) {
              const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
              ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];
              Vector<int> pin_indices_vec;
              for (const int points_i : chunk.points_range) {
                const bool is_pinned = pin_attr_span[points_i];
                if (!is_pinned) {
                  continue;
                }
                pin_indices_vec.append(points_i);
              }
              const int pin_num = pin_indices_vec.size();
              if (pin_num == 0) {
                continue;
              }
              const Span<int> pin_indices = thread_allocator.construct_array_copy<int>(
                  pin_indices_vec);
              MutableSpan<float> compliance_terms = thread_allocator.allocate_array<float>(
                  pin_num);
              MutableSpan<float> lambdas = thread_allocator.allocate_array<float>(pin_num);
              for (const int pin_i : IndexRange(pin_num)) {
                const int point_i = pin_indices[pin_i];
                compliance_terms[pin_i] = compliance_attr[point_i] * substep_compliance_factor_;
                lambdas[pin_i] = geo_data.pin_position_lambda_attr.span[point_i];
              }
              /* This is initialized in each substep. */
              MutableSpan<float3> pin_positions = thread_allocator.allocate_array<float3>(pin_num);

              chunk_constraints.pin_position_indices = pin_indices;
              chunk_constraints.pin_positions = pin_positions;
              chunk_constraints.pin_position_lambdas = lambdas;
              chunk_constraints.static_constraints.append(
                  &thread_scope.construct<xpbd::PinPositionConstraintSet>(
                      data_key_i, pin_indices, pin_positions, compliance_terms, lambdas));
            }
          });
    }
  }

  void prepare_pinned_rotations()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      const bke::AttributeReader<bool> pin_attr = geo_data.attributes.lookup<bool>(
          attribute_names::sim_pin_rotation, geo_data.domain);
      const bke::AttributeReader<math::Quaternion> begin_attr =
          geo_data.attributes.lookup<math::Quaternion>(attribute_names::sim_pin_rotation_begin,
                                                       geo_data.domain);
      const bke::AttributeReader<math::Quaternion> end_attr =
          geo_data.attributes.lookup<math::Quaternion>(attribute_names::sim_pin_rotation_end,
                                                       geo_data.domain);
      if (!pin_attr || !begin_attr || !end_attr) {
        continue;
      }

      const VArraySpan<bool> pin_attr_span = *pin_attr;
      geo_data.pin_rotation_begin = *begin_attr;
      geo_data.pin_rotation_end = *end_attr;
      geo_data.pin_rotation_lambda_attr =
          geo_data.attributes.lookup_or_add_for_write_span<math::Quaternion>(
              attribute_names::sim_pin_rotation_lambda,
              geo_data.domain,
              bke::AttributeInitValue(math::Quaternion(0, 0, 0, 0)));

      const VArray<float> compliance_attr = *geo_data.attributes.lookup_or_default<float>(
          attribute_names::sim_pin_rotation_compliance, geo_data.domain, 0.0f);

      threading::parallel_for(
          geo_data.chunks.index_range(), 16, [&](const IndexRange chunk_range) {
            ResourceScope &thread_scope = thread_scopes_.local();
            LinearAllocator<> &thread_allocator = thread_scope.allocator();
            for (const int chunk_i : chunk_range) {
              const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
              ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];
              Vector<int> pin_indices_vec;
              for (const int points_i : chunk.points_range) {
                const bool is_pinned = pin_attr_span[points_i];
                if (!is_pinned) {
                  continue;
                }
                pin_indices_vec.append(points_i);
              }
              const int pin_num = pin_indices_vec.size();
              if (pin_num == 0) {
                continue;
              }
              const Span<int> pin_indices = thread_allocator.construct_array_copy<int>(
                  pin_indices_vec);
              MutableSpan<float> compliance_terms = thread_allocator.allocate_array<float>(
                  pin_num);
              MutableSpan<float4> lambdas = thread_allocator.allocate_array<float4>(pin_num);
              for (const int pin_i : IndexRange(pin_num)) {
                const int point_i = pin_indices[pin_i];
                compliance_terms[pin_i] = compliance_attr[point_i] * substep_compliance_factor_;
                lambdas[pin_i] = float4(&geo_data.pin_rotation_lambda_attr.span[point_i].w);
              }
              /* This is initialized in each substep. */
              MutableSpan<math::Quaternion> pin_rotations =
                  thread_allocator.allocate_array<math::Quaternion>(pin_num);

              chunk_constraints.pin_rotation_indices = pin_indices;
              chunk_constraints.pin_rotations = pin_rotations;
              chunk_constraints.pin_rotation_lambdas = lambdas;
              chunk_constraints.static_constraints.append(
                  &thread_scope.construct<xpbd::PinRotationConstraintSet>(
                      data_key_i, pin_indices, pin_rotations, compliance_terms, lambdas));
            }
          });
    }
  }

  void prepare_inverse_masses()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      geo_data.inv_masses.reinitialize(geo_data.size);
      MutableSpan<float> inv_masses = geo_data.inv_masses;

      const bke::AttributeReader<float> mass_attr = geo_data.attributes.lookup<float>(
          attribute_names::mass, geo_data.domain);

      if (mass_attr) {
        threading::parallel_for(IndexRange(geo_data.size), 2048, [&](const IndexRange range) {
          for (const int i : range) {
            const float mass = mass_attr.varray[i];
            if (mass <= 0.0f) {
              inv_masses[i] = 1.0f;
            }
            else {
              inv_masses[i] = 1.0f / mass;
            }
          }
        });
      }
      else {
        inv_masses.fill(1.0f);
      }

      /* Pinned points have infinite mass, so their inverse mass is 0. */
      // TODO: This should only be done for hard pinning.
      // index_mask::masked_fill(inv_masses, 0.0f, geo_data.pin_position_mask);
    }
  }

  void prepare_inverse_moments_of_inertia()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      geo_data.inv_moments_of_inertia.reinitialize(geo_data.size);
      MutableSpan<float3> inv_moments_of_inertia = geo_data.inv_moments_of_inertia;

      const bke::AttributeReader<float3> moment_of_inertia_attr =
          geo_data.attributes.lookup<float3>(attribute_names::moment_of_inertia, geo_data.domain);

      if (moment_of_inertia_attr) {
        threading::parallel_for(IndexRange(geo_data.size), 2048, [&](const IndexRange range) {
          for (const int i : range) {
            const float3 moment_of_inertia = moment_of_inertia_attr.varray[i];
            if (math::is_zero(moment_of_inertia)) {
              inv_moments_of_inertia[i] = float3(0.0f);
            }
            else {
              inv_moments_of_inertia[i] = math::safe_rcp(moment_of_inertia);
            }
          }
        });
      }
      else {
        inv_moments_of_inertia.fill(float3(1.0f));
      }

      // TODO: This should only be done for hard pinning.
      // index_mask::masked_fill(inv_inertias, float3(0.0f), geo_data.pin_rotation_mask);

      geo_data.moments_of_inertia.reinitialize(geo_data.size);
      for (const int i : IndexRange(geo_data.size)) {
        const float3 &inv_inertia = inv_moments_of_inertia[i];
        if (math::is_zero(inv_inertia)) {
          geo_data.moments_of_inertia[i] = float3(std::numeric_limits<float>::infinity());
        }
        else {
          geo_data.moments_of_inertia[i] = math::safe_rcp(inv_inertia);
        }
      }
    }
  }

  void gather_from_world__stretch_shear_constraints()
  {
    const Vector<std::string> paths = gather_bundle_paths_by_type(
        world_, RodStretchAndShearXPBDConstraintBundle::name);
    for (const StringRef path : paths) {
      const BundlePtr *bundle_ptr = world_.lookup_path_ptr<BundlePtr>(path);
      if (!bundle_ptr || !*bundle_ptr) {
        continue;
      }
      const Bundle &bundle = **bundle_ptr;
      const Field<float> compliance_field =
          bundle.lookup<Field<float>>("compliance").value_or(fn::make_constant_field(0.0f));

      const int constraint_i = constraints_info_.rod_stretch_shear_constraints
                                   .append_and_get_index({path, compliance_field});
      for (const int data_key_i : geometries_.data_keys.index_range()) {
        GeometryData &geo_data = geometries_.data[data_key_i];
        if (!geo_data.curves) {
          continue;
        }
        if (this->behavior_applies_to_geometry(path, bundle, data_key_i)) {
          if (geo_data.rod_stretch_shear_constraint.has_value()) {
            this->report_warning(RPT_("Duplicate rod stretch/shear constraint"));
            break;
          }
          geo_data.rod_stretch_shear_constraint = {constraint_i};
        }
      }

      for (const int data_key_i : geometries_.data_keys.index_range()) {
        GeometryData &geo_data = geometries_.data[data_key_i];
        if (!geo_data.rod_stretch_shear_constraint.has_value()) {
          continue;
        }
        RodStretchShearConstraintUsage &constraint_usage = *geo_data.rod_stretch_shear_constraint;
        const RodStretchShearConstraint &constraint =
            constraints_info_.rod_stretch_shear_constraints[constraint_usage.constraint_i];

        constraint_usage.lambdas_pos = global_allocator_.allocate_array<float3>(geo_data.size);
        constraint_usage.lambdas_rot = global_allocator_.allocate_array<float3>(geo_data.size);

        fn::FieldEvaluator &evaluator = this->get_field_evaluator(data_key_i, geo_data.domain);
        evaluator.add(constraint.compliance, &constraint_usage.compliances);
      }
    }
  }

  void create_constraints__rod_stretch_shear()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      if (!geo_data.rod_stretch_shear_constraint.has_value()) {
        continue;
      }
      RodStretchShearConstraintUsage &constraint_usage = *geo_data.rod_stretch_shear_constraint;
      bke::CurvesGeometry &curves = *geo_data.curves;
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();

      const VArraySpanGetter<float> compliances{
          global_scope_, constraint_usage.compliances, geometries_.max_chunk_size};

      for (const int chunk_i : geo_data.chunks) {
        const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
        ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];
        chunk_constraints.static_constraints.append(
            &global_scope_.construct<xpbd::RodStretchAndShearConstraintSet>(
                data_key_i,
                *chunk.curves_range,
                points_by_curve,
                geo_data.rest_lengths,
                compliances.get_span_for_range(chunk.points_range),
                constraint_usage.lambdas_pos,
                constraint_usage.lambdas_rot));
      }
    }
  }

  void gather_from_world__bend_twist_constraints()
  {
    const Vector<std::string> paths = gather_bundle_paths_by_type(
        world_, RodBendAndTwistXPBDConstraintBundle::name);
    for (const StringRef path : paths) {
      const BundlePtr *bundle_ptr = world_.lookup_path_ptr<BundlePtr>(path);
      if (!bundle_ptr || !*bundle_ptr) {
        continue;
      }
      const Bundle &bundle = **bundle_ptr;
      const Field<float> compliance_field =
          bundle.lookup<Field<float>>("compliance").value_or(fn::make_constant_field(0.0f));

      for (const int data_key_i : geometries_.data_keys.index_range()) {
        GeometryData &geo_data = geometries_.data[data_key_i];
        if (geo_data.has_rod_bend_twist_constraint) {
          continue;
        }
        if (!this->behavior_applies_to_geometry(path, bundle, data_key_i)) {
          continue;
        }
        if (!geo_data.curves) {
          continue;
        }
        fn::FieldEvaluator &evaluator = this->get_field_evaluator(data_key_i, geo_data.domain);
        evaluator.add(compliance_field, &geo_data.rod_bend_twist_compliances);
        geo_data.has_rod_bend_twist_constraint = true;
      }
    }
  }

  void create_constraints__rod_bend_twist()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      if (!geo_data.has_rod_bend_twist_constraint) {
        continue;
      }
      bke::CurvesGeometry &curves = *geo_data.curves;
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();

      geo_data.rod_bend_twist_lambdas = global_allocator_.allocate_array<float4>(geo_data.size);
      const VArraySpanGetter<float> compliances{
          global_scope_, geo_data.rod_bend_twist_compliances, geometries_.max_chunk_size};

      for (const int chunk_i : geo_data.chunks) {
        const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
        constraints_info_.chunk_constraints[chunk_i].static_constraints.append(
            &global_scope_.construct<xpbd::RodBendAndTwistConstraintSet>(
                data_key_i,
                *chunk.curves_range,
                points_by_curve,
                geo_data.rest_bend_rotations,
                compliances.get_span_for_range(chunk.points_range),
                geo_data.rod_bend_twist_lambdas));
      }
    }
  }

  void gather_from_world__damping()
  {
    const Vector<std::string> paths = gather_bundle_paths_by_type(world_, DampingBundle::name);
    for (const StringRef path : paths) {
      const BundlePtr *bundle_ptr = world_.lookup_path_ptr<BundlePtr>(path);
      if (!bundle_ptr || !*bundle_ptr) {
        continue;
      }
      const Bundle &bundle = **bundle_ptr;
      const Field<float> linear_damping = this->get_field_or_constant<float>(
          bundle, "linear_damping", 0.0f);
      const Field<float> angular_damping = this->get_field_or_constant<float>(
          bundle, "angular_damping", 0.0f);

      const int constraint_i = constraints_info_.damping_constraints.append_and_get_index(
          {path, linear_damping, angular_damping});

      for (const int data_key_i : geometries_.data_keys.index_range()) {
        GeometryData &geo_data = geometries_.data[data_key_i];
        if (this->behavior_applies_to_geometry(path, bundle, data_key_i)) {
          geo_data.damping_constraints.append({constraint_i});
        }
      }
    }

    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      for (DampingConstraintUsage &constraint_usage : geo_data.damping_constraints) {
        const DampingConstraint &constraint =
            constraints_info_.damping_constraints[constraint_usage.constraint_i];
        constraint_usage.linear_damping_lambdas = global_allocator_.allocate_array<float>(
            geo_data.size);
        constraint_usage.angular_damping_lambdas = global_allocator_.allocate_array<float>(
            geo_data.size);
        fn::FieldEvaluator &evaluator = this->get_field_evaluator(data_key_i, geo_data.domain);
        evaluator.add(constraint.linear_damping, &constraint_usage.linear_dampings);
        evaluator.add(constraint.angular_damping, &constraint_usage.angular_dampings);
      }
    }
  }

  void create_constraints__damping()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];

      for (DampingConstraintUsage &constraint_usage : geo_data.damping_constraints) {
        const VArraySpanGetter<float> linear_dampings{
            global_scope_, constraint_usage.linear_dampings, geometries_.max_chunk_size};
        const VArraySpanGetter<float> angular_dampings{
            global_scope_, constraint_usage.angular_dampings, geometries_.max_chunk_size};

        for (const int chunk_i : geo_data.chunks) {
          const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
          ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];

          chunk_constraints.static_velocity_constraints.append(
              &global_scope_.construct<xpbd::LinearDampingConstraintSet>(
                  data_key_i,
                  chunk.points_range,
                  linear_dampings.get_span_for_range(chunk.points_range),
                  constraint_usage.linear_damping_lambdas.slice(chunk.points_range)));
          chunk_constraints.static_velocity_constraints.append(
              &global_scope_.construct<xpbd::AngularDampingConstraintSet>(
                  data_key_i,
                  chunk.points_range,
                  angular_dampings.get_span_for_range(chunk.points_range),
                  constraint_usage.angular_damping_lambdas.slice(chunk.points_range)));
        }
      }
    }
  }

  template<typename T>
  Field<T> get_field_or_constant(const Bundle &bundle,
                                 const StringRef name,
                                 const T &default_value)
  {
    const std::optional<Field<T>> field = bundle.lookup<Field<T>>(name);
    if (field) {
      return *field;
    }
    return fn::make_constant_field(default_value);
  }

  fn::FieldEvaluator &get_field_evaluator(const int data_key_i,
                                          const AttrDomain domain,
                                          std::optional<Field<bool>> selection = std::nullopt)
  {
    FieldEvaluatorKey key{data_key_i, domain, selection ? *selection : get_constant_true_field()};
    return *field_evaluators_.lookup_or_add_cb(key, [&]() {
      const auto &field_context = this->make_geometry_field_context(data_key_i, domain);
      const int domain_size = geometries_.data[data_key_i].size;
      auto &evaluator = global_scope_.construct<fn::FieldEvaluator>(field_context, domain_size);
      if (selection) {
        evaluator.set_selection(*selection);
      }
      return &evaluator;
    });
  }

  Field<float> get_compliance_term_field(const Bundle &bundle, const StringRef field_name) const
  {
    const std::optional<Field<float>> compliance_term = bundle.lookup<Field<float>>(field_name);
    if (!compliance_term) {
      static auto zero_field = fn::make_constant_field(0.0f);
      return zero_field;
    }
    return this->to_compliance_term_field(*compliance_term);
  }

  Field<float> to_compliance_term_field(const Field<float> &compliance_field) const
  {
    static auto prepare_compliance_term_fn = mf::build::SI2_SO<float, float, float>(
        "Prepare Compliance Term", [](const float compliance, const float factor) {
          return std::max(0.0f, compliance * factor);
        });
    return Field<float>(fn::FieldOperation::from(
        prepare_compliance_term_fn, {compliance_field, substep_compliance_factor_field_}));
  }

  fn::FieldContext &make_geometry_field_context(const int data_key_i, const AttrDomain domain)
  {
    const DataKey &data_key = geometries_.data_keys[data_key_i];
    const GeometrySet &geometry_set = geometries_.geometry_sets[data_key.geo_bundle_i].geometry;
    switch (data_key.type) {
      case bke::GeometryComponent::Type::Mesh:
        return global_scope_.construct<bke::MeshFieldContext>(*geometry_set.get_mesh(), domain);
      case bke::GeometryComponent::Type::PointCloud:
        return global_scope_.construct<bke::PointCloudFieldContext>(
            *geometry_set.get_pointcloud());
      case bke::GeometryComponent::Type::Instance:
        return global_scope_.construct<bke::InstancesFieldContext>(*geometry_set.get_instances());
      case bke::GeometryComponent::Type::Curve:
        return global_scope_.construct<bke::CurvesFieldContext>(*geometry_set.get_curves(),
                                                                domain);
      case bke::GeometryComponent::Type::GreasePencil:
        return global_scope_.construct<bke::GreasePencilLayerFieldContext>(
            *geometry_set.get_grease_pencil(), domain, *data_key.layer_i);
      case bke::GeometryComponent::Type::Volume:
      case bke::GeometryComponent::Type::Edit:
        break;
    }
    BLI_assert_unreachable();
    return global_scope_.construct<fn::FieldContext>();
  }

  Vector<int> find_data_keys_for_filter(const StringRef self_path, const StringRef filter) const
  {
    Vector<int> data_keys;
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      const DataKey &data_key = geometries_.data_keys[data_key_i];
      const StringRef geo_bundle_path = geometries_.geometry_sets[data_key.geo_bundle_i].path;
      if (nested_bundle_path_is_selected(self_path, filter, geo_bundle_path)) {
        data_keys.append(data_key_i);
      }
    }
    return data_keys;
  }

  void evaluate_constraint_fields()
  {
    Vector<fn::FieldEvaluator *> evaluators;
    for (fn::FieldEvaluator *evaluator : field_evaluators_.values()) {
      evaluators.append(evaluator);
    }
    threading::parallel_for(
        evaluators.index_range(),
        1024,
        [&](const IndexRange range) {
          for (fn::FieldEvaluator *evaluator : evaluators.as_span().slice(range)) {
            evaluator->evaluate();
          }
        },
        threading::individual_task_sizes(
            [&](const int i) { return evaluators[i]->evaluation_mask().size(); }));
  }

  void do_simulation()
  {
    this->prepare_solver_geometry_refs();

    if (this->support_chunk_local_simulation()) {
      this->parallel_for_each_chunk(1, [&](const int chunk_i) {
        int solver_refs_i = 0;
        for (const int substep_i : IndexRange(substeps_)) {
          const SubstepInterval substep(substeps_, substep_i);
          this->simulate__update_pin_positions__chunk(chunk_i, substep);
          this->simulate__inertial_update__chunk(chunk_i, solver_refs_i);
          this->simulate__gather_dynamic_constraints__chunk(substep, chunk_i);
          this->simulate__reset_forces__chunk(chunk_i);
          for ([[maybe_unused]] const int iter_i : IndexRange(constraint_iterations_)) {
            this->simulate__position_solve__single_iteration__chunk(chunk_i, solver_refs_i);
          }
          this->simulate__update_velocities__chunk(chunk_i, solver_refs_i);
          this->simulate__velocity_solve__chunk(chunk_i, solver_refs_i);
          solver_refs_i = 1 - solver_refs_i;
        }
        this->simulate__ensure_final_data_in_outputs__chunk(chunk_i);
      });
    }
    else {
      int solver_refs_i = 0;
      for (const int substep_i : IndexRange(substeps_)) {
        const SubstepInterval substep(substeps_, substep_i);
        this->parallel_for_each_chunk(16, [&](const int chunk_i) {
          this->simulate__update_pin_positions__chunk(chunk_i, substep);
          this->simulate__inertial_update__chunk(chunk_i, solver_refs_i);
        });
        this->simulate__gather_dynamic_constraints(substep);
        this->simulate__reset_forces();
        for ([[maybe_unused]] const int iter_i : IndexRange(constraint_iterations_)) {
          this->simulate__position_solve__single_iteration(solver_refs_i);
        }
        this->parallel_for_each_chunk(16, [&](const int chunk_i) {
          this->simulate__update_velocities__chunk(chunk_i, solver_refs_i);
        });
        this->simulate__velocity_solve(solver_refs_i);
        this->parallel_for_each_chunk(16, [&](const int chunk_i) {
          this->simulate__ensure_final_data_in_outputs__chunk(chunk_i);
        });
        solver_refs_i = 1 - solver_refs_i;
      }
    }
  }

  bool support_chunk_local_simulation() const
  {
    return true;
  }

  void prepare_solver_geometry_refs()
  {
    for (const int direction : IndexRange(2)) {
      Array<xpbd::GeometryRef> &refs = geometries_.solver_refs[direction];
      refs.reinitialize(geometries_.data_keys.size());
      for (const int data_key_i : geometries_.data_keys.index_range()) {
        GeometryData &geo_data = geometries_.data[data_key_i];
        xpbd::GeometryRef &ref = refs[data_key_i];

        if (direction == 0) {
          ref.positions = geo_data.temp_positions;
          ref.rotations = geo_data.temp_rotations;
          ref.prev_positions = geo_data.position_attr.span;
          ref.prev_rotations = geo_data.rotation_attr.span;
        }
        else {
          ref.positions = geo_data.position_attr.span;
          ref.rotations = geo_data.rotation_attr.span;
          ref.prev_positions = geo_data.temp_positions;
          ref.prev_rotations = geo_data.temp_rotations;
        }

        ref.velocities = geo_data.velocity_attr.span;
        ref.inverse_masses = geo_data.inv_masses;
        ref.angular_velocities = geo_data.angular_velocity_attr.span;
        ref.moments_of_inertia = geo_data.moments_of_inertia;
        ref.inverse_moments_of_inertia = geo_data.inv_moments_of_inertia;
      }
    }
  }

  void simulate__update_pin_positions__chunk(const int chunk_i, const SubstepInterval &substep)
  {
    const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
    const int data_key_i = chunk.data_key_i;
    GeometryData &geo_data = geometries_.data[data_key_i];
    ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];

    /* Update animated pin positions. */
    for (const int i : chunk_constraints.pin_positions.index_range()) {
      const int point_i = chunk_constraints.pin_position_indices[i];
      const float3 &begin_pos = geo_data.pin_position_begin[point_i];
      const float3 &end_pos = geo_data.pin_position_end[point_i];
      const float3 pin_pos = math::interpolate(begin_pos, end_pos, substep.end_factor);
      chunk_constraints.pin_positions[i] = pin_pos;
    }
    for (const int i : chunk_constraints.pin_rotations.index_range()) {
      const int point_i = chunk_constraints.pin_rotation_indices[i];
      const math::Quaternion &begin_rot = geo_data.pin_rotation_begin[point_i];
      const math::Quaternion &end_rot = geo_data.pin_rotation_end[point_i];
      const math::Quaternion pin_rot = math::interpolate(begin_rot, end_rot, substep.end_factor);
      chunk_constraints.pin_rotations[i] = pin_rot;
    }
  }

  void simulate__inertial_update__chunk(const int chunk_i, const int solver_refs_i)
  {
    const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
    const IndexRange points_range = chunk.points_range;
    const int data_key_i = chunk.data_key_i;
    GeometryData &geo_data = geometries_.data[data_key_i];
    xpbd::GeometryRef &ref = geometries_.solver_refs[solver_refs_i][data_key_i];

    this->integrate_linear_velocities(sub_delta_time_,
                                      ref.prev_positions.slice(points_range),
                                      ref.positions.slice(points_range),
                                      geo_data.velocity_attr.span.slice(points_range),
                                      geo_data.inv_masses.as_span().slice(points_range),
                                      geo_data.external_force_attr.slice(points_range));
    this->integrate_angular_velocities(
        sub_delta_time_,
        ref.prev_rotations.slice(points_range),
        ref.rotations.slice(points_range),
        geo_data.angular_velocity_attr.span.slice(points_range),
        geo_data.inv_moments_of_inertia.as_span().slice(points_range),
        geo_data.external_torque_attr.slice(points_range));
  }

  void simulate__gather_dynamic_constraints(const SubstepInterval &substep)
  {
    this->parallel_for_each_chunk(1, [&](const int chunk_i) {
      this->simulate__gather_dynamic_constraints__chunk(substep, chunk_i);
    });
  }

  void simulate__gather_dynamic_constraints__chunk(const SubstepInterval &substep,
                                                   const int chunk_i)
  {
    const float max_distance = this->get_max_search_distance(sub_delta_time_);
    ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];
    const ExternalPlaneContacts &prev_contacts = chunk_constraints.external_plane_contacts;
    ExternalPlaneContacts new_contacts;
    this->gather_ground_plane_contacts(
        chunk_i, max_distance, substep, prev_contacts, new_contacts);
    this->gather_mesh_contacts(chunk_i, max_distance, substep, prev_contacts, new_contacts);

    const int contacts_num = new_contacts.points.size();
    for (const int i : IndexRange(contacts_num)) {
      new_contacts.collider_velocities.append(
          math::safe_divide(new_contacts.collider_motion[i], sub_delta_time_));
    }
    chunk_constraints.external_plane_contacts = std::move(new_contacts);
  }

  void simulate__reset_forces()
  {
    this->parallel_for_each_chunk(
        16, [&](const int chunk_i) { this->simulate__reset_forces__chunk(chunk_i); });
  }

  void simulate__reset_forces__chunk(const int chunk_i)
  {
    ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];
    for (xpbd::ConstraintSet *constraint : chunk_constraints.static_constraints) {
      constraint->reset_forces();
    }
    for (xpbd::VelocityConstraintSet *constraint : chunk_constraints.static_velocity_constraints) {
      constraint->reset_forces();
    }
  }

  void simulate__position_solve__single_iteration(const int solver_refs_i)
  {
    this->parallel_for_each_chunk(1, [&](const int chunk_i) {
      this->simulate__position_solve__single_iteration__chunk(chunk_i, solver_refs_i);
    });
  }

  void simulate__position_solve__single_iteration__chunk(const int chunk_i,
                                                         const int solver_refs_i)
  {
    const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
    ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];

    Vector<xpbd::ConstraintSet *> local_constraints = chunk_constraints.static_constraints;

    std::optional<xpbd::CollisionPlaneConstraintSet> plane_collision_constraint;
    if (!chunk_constraints.external_plane_contacts.points.is_empty()) {
      ExternalPlaneContacts &contacts = chunk_constraints.external_plane_contacts;
      plane_collision_constraint.emplace(chunk.data_key_i,
                                         contacts.points,
                                         contacts.positions_on_plane,
                                         contacts.collider_motion,
                                         contacts.separating_axes,
                                         contacts.compliance_terms,
                                         contacts.static_frictions,
                                         contacts.dynamic_frictions,
                                         contacts.active_states,
                                         contacts.lambdas_normal);
      local_constraints.append(&*plane_collision_constraint);
    }

    const Span<xpbd::GeometryRef> solver_refs = geometries_.solver_refs[solver_refs_i];
    xpbd::ConstraintSetParams solve_params{solver_refs, sub_delta_time_, std::nullopt};
    this->solve_constraints(solve_params, local_constraints);
  }

  void simulate__update_velocities__chunk(const int chunk_i, const int solver_refs_i)
  {
    const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
    const IndexRange points_range = chunk.points_range;
    const int data_key_i = chunk.data_key_i;
    GeometryData &geo_data = geometries_.data[data_key_i];
    xpbd::GeometryRef &ref = geometries_.solver_refs[solver_refs_i][data_key_i];
    this->update_linear_velocities(sub_delta_time_,
                                   ref.prev_positions.slice(points_range),
                                   ref.positions.slice(points_range),
                                   geo_data.velocity_attr.span.slice(points_range));
    this->update_angular_velocities(sub_delta_time_,
                                    ref.prev_rotations.slice(points_range),
                                    ref.rotations.slice(points_range),
                                    geo_data.angular_velocity_attr.span.slice(points_range));
  }

  void simulate__velocity_solve(const int solver_refs_i)
  {
    this->parallel_for_each_chunk(8, [&](const int chunk_i) {
      this->simulate__velocity_solve__chunk(chunk_i, solver_refs_i);
    });
  }

  void simulate__velocity_solve__chunk(const int chunk_i, const int solver_refs_i)
  {
    const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
    ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];

    Vector<xpbd::VelocityConstraintSet *> local_constraints =
        chunk_constraints.static_velocity_constraints;
    std::optional<xpbd::FrictionConstraintSet> friction_constraint;
    if (!chunk_constraints.external_plane_contacts.points.is_empty()) {
      ExternalPlaneContacts &contacts = chunk_constraints.external_plane_contacts;
      friction_constraint.emplace(chunk.data_key_i,
                                  contacts.points,
                                  contacts.separating_axes,
                                  contacts.collider_velocities,
                                  contacts.dynamic_frictions,
                                  contacts.lambdas_normal,
                                  contacts.lambdas);
      local_constraints.append(&*friction_constraint);
    }

    const Span<xpbd::GeometryRef> solver_refs = geometries_.solver_refs[solver_refs_i];
    xpbd::VelocityUpdater velocity_updater{solver_refs};
    xpbd::ConstraintSetParams params{solver_refs, sub_delta_time_, std::nullopt};
    for (xpbd::VelocityConstraintSet *constraint : local_constraints) {
      constraint->solve_step(velocity_updater, params);
    }
  }

  void simulate__ensure_final_data_in_outputs__chunk(const int chunk_i)
  {
    if (substeps_ % 2 == 0) {
      /* Nothing to do. */
      return;
    }
    const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
    GeometryData &geo_data = geometries_.data[chunk.data_key_i];
    const IndexRange points_range = chunk.points_range;
    geo_data.position_attr.span.slice(points_range)
        .copy_from(geo_data.temp_positions.as_span().slice(points_range));
  }

  void finish_attribute_writers()
  {
    this->parallel_for_each_chunk(16, [&](const int chunk_i) {
      const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
      const ChunkConstraints &chunk_constraints = constraints_info_.chunk_constraints[chunk_i];
      GeometryData &geo_data = geometries_.data[chunk.data_key_i];

      /* Write back pin position lambdas. */
      for (const int pin_i : chunk_constraints.pin_positions.index_range()) {
        const int point_i = chunk_constraints.pin_position_indices[pin_i];
        geo_data.pin_position_lambda_attr.span[point_i] =
            chunk_constraints.pin_position_lambdas[pin_i];
      }
      /* Write back pin rotation lambdas. */
      for (const int pin_i : chunk_constraints.pin_rotations.index_range()) {
        const int point_i = chunk_constraints.pin_rotation_indices[pin_i];
        geo_data.pin_rotation_lambda_attr.span[point_i] = math::Quaternion(
            chunk_constraints.pin_rotation_lambdas[pin_i]);
      }
    });

    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];

      geo_data.position_attr.finish();
      geo_data.velocity_attr.finish();
      geo_data.rotation_attr.finish();
      geo_data.angular_velocity_attr.finish();
      geo_data.pin_position_lambda_attr.finish();
      geo_data.pin_rotation_lambda_attr.finish();
    }
  }

  void integrate_linear_velocities(const float delta_time,
                                   const Span<float3> old_positions,
                                   MutableSpan<float3> new_positions,
                                   MutableSpan<float3> velocities,
                                   const Span<float> inv_masses,
                                   const Span<float3> forces)
  {
    for (const int i : old_positions.index_range()) {
      const float3 external_force = forces[i];
      const float inv_mass = inv_masses[i];
      const float3 &old_pos = old_positions[i];
      const float3 acceleration = external_force * inv_mass;
      const float3 new_velocity = velocities[i] + acceleration * delta_time;
      velocities[i] = new_velocity;
      new_positions[i] = old_pos + new_velocity * delta_time;
    }
  }

  void integrate_angular_velocities(const float delta_time,
                                    const Span<math::Quaternion> old_rotations,
                                    MutableSpan<math::Quaternion> new_rotations,
                                    MutableSpan<float3> angular_velocities,
                                    const Span<float3> inv_inertias,
                                    const Span<float3> torques)
  {
    for (const int i : old_rotations.index_range()) {
      const math::Quaternion &old_rotation = old_rotations[i];
      const float3 &external_torque = torques[i];
      const float3 &inv_inertia = inv_inertias[i];
      if (math::is_zero(inv_inertia)) {
        new_rotations[i] = old_rotation;
        continue;
      }
      float3 &angular_velocity = angular_velocities[i];
      const float3 precession = math::cross(angular_velocity,
                                            math::safe_divide(angular_velocity, inv_inertia));
      angular_velocity += delta_time * (external_torque - precession) * inv_inertia;
      const math::Quaternion direction = old_rotation * math::Quaternion(0, angular_velocity);
      new_rotations[i] = math::normalize(
          math::Quaternion(float4(old_rotation) + delta_time * 0.5f * float4(direction)));
    }
  }

  void update_linear_velocities(const float delta_time,
                                const Span<float3> prev_positions,
                                const Span<float3> new_positions,
                                MutableSpan<float3> r_velocities)
  {
    const float inv_delta_time = math::safe_rcp(delta_time);
    for (const int i : r_velocities.index_range()) {
      const float3 &old_pos = prev_positions[i];
      const float3 &new_pos = new_positions[i];
      const float3 diff = new_pos - old_pos;
      const float3 velocity = diff * inv_delta_time;
      r_velocities[i] = velocity;
    }
  }

  void update_angular_velocities(const float delta_time,
                                 const Span<math::Quaternion> prev_rotations,
                                 const Span<math::Quaternion> new_rotations,
                                 MutableSpan<float3> r_angular_velocities)
  {
    const float inv_delta_time = math::safe_rcp(delta_time);
    for (const int i : r_angular_velocities.index_range()) {
      float3 diff =
          (math::invert_normalized(prev_rotations[i]) * new_rotations[i]).imaginary_part();
      for (const int j : IndexRange(3)) {
        if (math::abs(diff[j]) < 1e-5f) {
          diff[j] = 0.0f;
        }
      }
      const float3 new_angular_velocity = 2.0f * diff * inv_delta_time;
      r_angular_velocities[i] = new_angular_velocity;
    }
  }

  void solve_constraints(xpbd::ConstraintSetParams &params,
                         const Span<xpbd::ConstraintSet *> constraint_sets)
  {
    switch (solver_type_) {
      case SolverType::SerialGaussSeidel: {
        xpbd::solve_gauss_seidel_one_at_a_time(params, constraint_sets);
        break;
      }
      case SolverType::ParallelGaussSeidel: {
        xpbd::solve_gauss_seidel_parallel(params, constraint_sets);
        break;
      }
      case SolverType::NonDeterministicJacobian: {
        xpbd::solve_jacobian_non_deterministic(params, constraint_sets);
        break;
      }
    }
  }

  void gather_ground_plane_contacts(const int chunk_i,
                                    const float max_distance,
                                    const SubstepInterval &substep,
                                    const ExternalPlaneContacts &prev_contacts,
                                    ExternalPlaneContacts &r_contacts)
  {
    const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
    const GeometryData &geo_data = geometries_.data[chunk.data_key_i];
    for (const InfinitePlaneColliderUsage &collider_usage : geo_data.infinite_plane_colliders) {
      const InfinitePlaneCollider &collider =
          constraints_info_.infinite_plane_colliders[collider_usage.constraint_i];
      const float3 collider_position = math::interpolate(
          collider.begin_position, collider.end_position, substep.end_factor);
      const float3 collider_normal = math::interpolate(
          collider.begin_normal, collider.end_normal, substep.end_factor);
      for (const int point_i : chunk.points_range) {
        const float3 &position = geo_data.position_attr.span[point_i];

        const float distance = math::dot(position - collider_position, collider_normal);
        if (distance >= max_distance) {
          continue;
        }

        const int contact_i = r_contacts.points.append_and_get_index(point_i);
        r_contacts.positions_on_plane.append(position - collider_normal * distance);
        /* Static plane does not move. */
        r_contacts.collider_motion.append(float3(0.0f));
        r_contacts.separating_axes.append(collider_normal);
        const float point_friction = geo_data.frictions[point_i];
        const float friction = this->compute_contact_friction(point_friction, collider.friction);
        r_contacts.static_frictions.append(friction);
        r_contacts.dynamic_frictions.append(friction);
        r_contacts.compliance_terms.append(0.0f);

        const InfinitePlaneContactId contact_id{collider_usage.constraint_i, point_i};
        r_contacts.infinite_plane_contact_indices.add(contact_id, contact_i);
        r_contacts.init_or_preserve_state(
            prev_contacts, prev_contacts.infinite_plane_contact_indices.lookup_try(contact_id));
      }
    }
  }

  void gather_mesh_contacts(const int chunk_i,
                            const float max_distance,
                            const SubstepInterval &substep,
                            const ExternalPlaneContacts &prev_contacts,
                            ExternalPlaneContacts &r_contacts)
  {
    const GeometryDataChunk &chunk = geometries_.chunks[chunk_i];
    const GeometryData &geo_data = geometries_.data[chunk.data_key_i];
    for (const MeshColliderUsage &collider_usage : geo_data.mesh_colliders) {
      const MeshCollider &collider = constraints_info_.mesh_colliders[collider_usage.constraint_i];
      const bke::BVHTreeFromMesh &bvh = collider.corner_tris_bvh;
      const float4x4 &mesh_to_local = math::interpolate(
          collider.begin_transform, collider.end_transform, substep.end_factor);
      const float4x4 &prev_mesh_to_local = math::interpolate(
          collider.begin_transform, collider.end_transform, substep.begin_factor);
      const float4x4 local_to_mesh = math::invert(mesh_to_local);

      for (const int point_i : chunk.points_range) {
        const float3 &pos_local = geo_data.position_attr.span[point_i];
        const float3 pos_mesh = math::transform_point(local_to_mesh, pos_local);
        BVHTreeNearest nearest{};
        nearest.index = -1;
        nearest.dist_sq = pow2f(max_distance);
        BLI_bvhtree_find_nearest(bvh.tree, pos_mesh, &nearest, bvh.nearest_callback, (void *)&bvh);
        if (nearest.index == -1) {
          continue;
        }

        const float3 &contact_pos_mesh = float3(nearest.co);
        const float3 dir_mesh = contact_pos_mesh - pos_mesh;
        const bool is_inside = math::dot(dir_mesh, float3(nearest.no)) > 0.0f;
        const float point_friction = geo_data.frictions[point_i];
        const float friction = this->compute_contact_friction(point_friction, collider.friction);
        const float3 contact_pos_local = math::transform_point(mesh_to_local, contact_pos_mesh);
        const float3 prev_contact_pos_local = math::transform_point(prev_mesh_to_local,
                                                                    contact_pos_mesh);
        /* Separating axis to move self out of penetration. */
        const float3 collision_axis = is_inside ? contact_pos_local - pos_local :
                                                  pos_local - contact_pos_local;
        const float3 valid_axis = math::normalize(math::is_zero(collision_axis, 1e-6f) ?
                                                      math::transpose(float3x3(local_to_mesh)) *
                                                          contact_pos_mesh :
                                                      collision_axis);
        const int contact_i = r_contacts.points.append_and_get_index(point_i);
        r_contacts.positions_on_plane.append(contact_pos_local);
        r_contacts.collider_motion.append(contact_pos_local - prev_contact_pos_local);
        r_contacts.separating_axes.append(valid_axis);
        r_contacts.static_frictions.append(friction);
        r_contacts.dynamic_frictions.append(friction);
        r_contacts.compliance_terms.append(
            std::max(0.0f, substep_compliance_factor_ * collider.compliance));

        const MeshContactId contact_id{collider_usage.constraint_i, point_i};
        r_contacts.mesh_contact_indices.add(contact_id, contact_i);
        r_contacts.init_or_preserve_state(
            prev_contacts, prev_contacts.mesh_contact_indices.lookup_try(contact_id));
      }
    }
  }

  float compute_contact_friction(const float point_friction, const float collider_friction) const
  {
    return math::sqrt(point_friction * collider_friction);
  }

  float get_max_search_distance(const float delta_time)
  {
    /* Slightly more than 200 km/h. */
    constexpr float max_velocity = 60.0f;
    const float max_search_distance = 2.0f * max_velocity * delta_time;
    return max_search_distance;
  }

  void parallel_for_each_chunk(const int grain_size, const FunctionRef<void(int chunk_i)> fn)
  {
    threading::parallel_for(
        geometries_.chunks.index_range(), grain_size, [&](const IndexRange chunk_range) {
          for (const int chunk_i : chunk_range) {
            fn(chunk_i);
          }
        });
  }

  void write_back_geometries_to_world()
  {
    for (const int geo_bundle_i : geometries_.geometry_sets.index_range()) {
      GeometrySetData &geo_set_data = geometries_.geometry_sets[geo_bundle_i];
      world_.add_path_override(geo_set_data.path + "/geometry", std::move(geo_set_data.geometry));
    }
  }

  void report_warning(std::string warning)
  {
    // TODO
    UNUSED_VARS(warning);
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr world_ptr = params.get_input<BundlePtr>("World");
  if (!world_ptr) {
    params.set_default_remaining_outputs();
    return;
  }
  const int substeps = params.get_input<int>("Substeps");
  if (substeps <= 0) {
    params.set_output("World", std::move(world_ptr));
    return;
  }
  const SolverType solver_type = params.get_input<SolverType>("Solver Type");
  const int constraint_iterations = params.get_input<int>("Constraint Iterations");
  const float delta_time = std::max(0.0f, params.get_input<float>("Delta Time"));
  Bundle &world = world_ptr.ensure_mutable_inplace();
  ResourceScope scope;

  XpbdSolverStep step(scope, world, delta_time, substeps, solver_type, constraint_iterations);
  step.do_step();

  params.set_output("World", std::move(world_ptr));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeXPBDSolver");
  ntype.ui_name = "XPBD Solver";
  ntype.ui_description = "Simulate physics using the XPBD framework";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Middle);
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_xpbd_solver_cc
