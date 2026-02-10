/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"

#include "BKE_curves.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_grease_pencil.hh"

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
/** Force that is applied to each point. */
constexpr StringRefNull external_force = "external_force";
constexpr StringRefNull external_torque = "external_torque";
/** Mass of each point. */
constexpr StringRefNull mass = "mass";
/* TODO: Should this be something like `rotational_inertia`? */
constexpr StringRefNull inertia = "inertia";

/** True for pinned points. */
constexpr StringRefNull sim_pin_position = "sim_pin_position";
/** Begin and end pin position for the current time step. The position is interpolated. */
constexpr StringRefNull sim_pin_position_begin = "sim_pin_position_begin";
constexpr StringRefNull sim_pin_position_end = "sim_pin_position_end";
/** Compliance of the pin position constraint. */
constexpr StringRefNull sim_pin_position_compliance = "sim_pin_position_compliance";

constexpr StringRefNull sim_pin_rotation = "sim_pin_rotation";
constexpr StringRefNull sim_pin_rotation_begin = "sim_pin_rotation_begin";
constexpr StringRefNull sim_pin_rotation_end = "sim_pin_rotation_end";
constexpr StringRefNull sim_pin_rotation_compliance = "sim_pin_rotation_compliance";

}  // namespace attribute_names

static NestedBundleTypePtr make_world_type()
{
  Vector<std::shared_ptr<const FlatBundleType>> types;
  types.append(GravityBundle::get_bundle_type());
  types.append(XPBDGeometryBundle::get_bundle_type());

  NestedBundleTypePtr world_type = std::make_shared<const NestedBundleType>("Blender.XpbdWorld",
                                                                            std::move(types));
  BundleTypeRegistry::register_type(world_type);
  return world_type;
}

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

struct GeometryData {
  bke::MutableAttributeAccessor attributes;
  AttrDomain domain;
  int size;

  IndexMask pin_position_mask;
  VArraySpan<float3> pin_position_begin;
  VArraySpan<float3> pin_position_end;

  IndexMask pin_rotation_mask;
  VArraySpan<math::Quaternion> pin_rotation_begin;
  VArraySpan<math::Quaternion> pin_rotation_end;

  /**
   * Inverse of the mass attribute + extra changes:
   * - For pinned positions this is set to 0.
   * - If there is no mass attribute, or it is <= 0, this is set to 1.
   */
  Array<float> inv_masses;

  Array<float3> inv_inertias;
};

struct Geometries {
  Vector<std::string> paths;
  Vector<GeometrySet> geometry_sets;

  VectorSet<DataKey> data_keys;
  Vector<GeometryData> data;
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

template<typename T> struct StartStopPair {
  T start;
  T stop;

  T interpolate(const float factor) const
  {
    return math::interpolate(start, stop, factor);
  }
};

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

struct ConstraintsInfo {};

class XpbdSolverStep {
 private:
  ResourceScope &scope_;
  IndexMaskMemory memory_;
  Bundle &world_;
  const float total_delta_time_;
  const int substeps_;
  const float sub_delta_time_;

  float substep_compliance_factor_;
  fn::Field<float> substep_compliance_factor_field_;

  Map<FieldEvaluatorKey, fn::FieldEvaluator *> field_evaluators_;

  Geometries geometries_;
  ConstraintsInfo constraints_info_;

 public:
  XpbdSolverStep(ResourceScope &scope,
                 Bundle &world,
                 const float total_delta_time,
                 const int substeps)
      : scope_(scope),
        world_(world),
        total_delta_time_(total_delta_time),
        substeps_(substeps),
        sub_delta_time_(total_delta_time / substeps_)
  {
  }

  void do_step()
  {
    this->prepare_substep_compliance_factor();
    this->gather_geometries_from_world();
    this->prepare_pinned_positions();
    this->prepare_pinned_rotations();
    this->prepare_inverse_masses();
    this->prepare_inverse_inertias();
    this->gather_constraints_from_world();
    this->evaluate_constraint_fields();
    this->prepare_constraints();
    this->do_simulation();
    this->write_back_geometries_to_world();
  }

 private:
  void prepare_substep_compliance_factor()
  {
    substep_compliance_factor_ = math::safe_rcp(pow2f(sub_delta_time_));
    substep_compliance_factor_field_ = fn::make_constant_field(substep_compliance_factor_);
  }

  void gather_geometries_from_world()
  {
    /* Gather geometry bundle paths. */
    geometries_.paths = gather_bundle_paths_by_type(world_, XPBDGeometryBundle::name);

    /* Move the geometry sets out of the bundles. They are put back in after the simulation. */
    geometries_.geometry_sets.reinitialize(geometries_.paths.size());
    for (const int i : geometries_.paths.index_range()) {
      const StringRef geometry_path = geometries_.paths[i];
      if (GeometrySet *geometry = world_.lookup_path_for_write_ptr<GeometrySet>(geometry_path +
                                                                                "/geometry"))
      {
        geometries_.geometry_sets[i] = std::move(*geometry);
      }
    }

    /* Gather individual components that should be simulated. There may be more than geometry sets
     * because each geometry set could contain e.g. a mesh and curves. */
    for (const int geo_bundle_i : geometries_.paths.index_range()) {
      GeometrySet &geometry = geometries_.geometry_sets[geo_bundle_i];
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
          geometries_.data.append(std::move(geo_data));
        }
      }
    }
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      geo_data.size = geo_data.attributes.domain_size(geo_data.domain);
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
      geo_data.pin_position_mask = IndexMask::from_bools(*pin_attr, memory_);
      geo_data.pin_position_begin = begin_attr.varray;
      geo_data.pin_position_end = end_attr.varray;
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
      geo_data.pin_rotation_mask = IndexMask::from_bools(*pin_attr, memory_);
      geo_data.pin_rotation_begin = begin_attr.varray;
      geo_data.pin_rotation_end = end_attr.varray;
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
      index_mask::masked_fill(inv_masses, 0.0f, geo_data.pin_position_mask);
    }
  }

  void prepare_inverse_inertias()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = geometries_.data[data_key_i];
      geo_data.inv_inertias.reinitialize(geo_data.size);
      MutableSpan<float3> inv_inertias = geo_data.inv_inertias;

      const bke::AttributeReader<float3> inertia_attr = geo_data.attributes.lookup<float3>(
          attribute_names::inertia, geo_data.domain);

      if (inertia_attr) {
        threading::parallel_for(IndexRange(geo_data.size), 2048, [&](const IndexRange range) {
          for (const int i : range) {
            const float3 inertia = inertia_attr.varray[i];
            if (math::is_zero(inertia)) {
              inv_inertias[i] = float3(0.0f);
            }
            else {
              inv_inertias[i] = math::safe_rcp(inertia);
            }
          }
        });
      }
      else {
        inv_inertias.fill(float3(1.0f));
      }

      index_mask::masked_fill(inv_inertias, float3(0.0f), geo_data.pin_rotation_mask);
    }
  }

  void gather_constraints_from_world() {}

  void prepare_constraints() {}

  fn::FieldEvaluator &get_field_evaluator(const int data_key_i,
                                          const AttrDomain domain,
                                          std::optional<Field<bool>> selection)
  {
    FieldEvaluatorKey key{data_key_i, domain, selection ? *selection : get_constant_true_field()};
    return *field_evaluators_.lookup_or_add_cb(key, [&]() {
      const auto &field_context = this->make_geometry_field_context(data_key_i, domain);
      const int domain_size = geometries_.data[data_key_i].size;
      auto &evaluator = scope_.construct<fn::FieldEvaluator>(field_context, domain_size);
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
    const GeometrySet &geometry_set = geometries_.geometry_sets[data_key.geo_bundle_i];
    switch (data_key.type) {
      case bke::GeometryComponent::Type::Mesh:
        return scope_.construct<bke::MeshFieldContext>(*geometry_set.get_mesh(), domain);
      case bke::GeometryComponent::Type::PointCloud:
        return scope_.construct<bke::PointCloudFieldContext>(*geometry_set.get_pointcloud());
      case bke::GeometryComponent::Type::Instance:
        return scope_.construct<bke::InstancesFieldContext>(*geometry_set.get_instances());
      case bke::GeometryComponent::Type::Curve:
        return scope_.construct<bke::CurvesFieldContext>(*geometry_set.get_curves(), domain);
      case bke::GeometryComponent::Type::GreasePencil:
        return scope_.construct<bke::GreasePencilLayerFieldContext>(
            *geometry_set.get_grease_pencil(), domain, *data_key.layer_i);
      case bke::GeometryComponent::Type::Volume:
      case bke::GeometryComponent::Type::Edit:
        break;
    }
    BLI_assert_unreachable();
    return scope_.construct<fn::FieldContext>();
  }

  Vector<int> find_data_keys_for_filter(const StringRef self_path, const StringRef filter) const
  {
    Vector<int> data_keys;
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      const DataKey &data_key = geometries_.data_keys[data_key_i];
      const StringRef geo_bundle_path = geometries_.paths[data_key.geo_bundle_i];
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
    for (const int substep_i : IndexRange(substeps_)) {
      const SubstepInterval substep(substeps_, substep_i);
      for (const int data_key_i : geometries_.data_keys.index_range()) {
        GeometryData &geo_data = geometries_.data[data_key_i];
        bke::SpanAttributeWriter<float3> position_attr =
            geo_data.attributes.lookup_or_add_for_write_span<float3>(attribute_names::position,
                                                                     geo_data.domain);
        bke::SpanAttributeWriter<float3> velocity_attr =
            geo_data.attributes.lookup_or_add_for_write_span<float3>(attribute_names::velocity,
                                                                     geo_data.domain);
        bke::SpanAttributeWriter<math::Quaternion> rotation_attr =
            geo_data.attributes.lookup_or_add_for_write_span<math::Quaternion>(
                attribute_names::rotation, geo_data.domain);
        bke::SpanAttributeWriter<float3> angular_velocity_attr =
            geo_data.attributes.lookup_or_add_for_write_span<float3>(
                attribute_names::angular_velocity, geo_data.domain);
        MutableSpan<float3> positions = position_attr.span;
        MutableSpan<float3> velocities = velocity_attr.span;
        MutableSpan<math::Quaternion> rotations = rotation_attr.span;
        MutableSpan<float3> angular_velocities = angular_velocity_attr.span;
        const VArraySpan<float3> external_forces = *geo_data.attributes.lookup_or_default<float3>(
            attribute_names::external_force, geo_data.domain, float3(0, 0, 0));
        const VArraySpan<float3> external_torques = *geo_data.attributes.lookup_or_default<float3>(
            attribute_names::external_torque, geo_data.domain, float3(0, 0, 0));
        const Span<float> inv_masses = geo_data.inv_masses;
        const Span<float3> inv_inertias = geo_data.inv_inertias;
        threading::parallel_for(IndexRange(geo_data.size), 256, [&](const IndexRange range) {
          this->integrate_linear_velocities(sub_delta_time_,
                                            positions.slice(range),
                                            positions.slice(range),
                                            velocities.slice(range),
                                            inv_masses.slice(range),
                                            external_forces.slice(range));
        });
        threading::parallel_for(IndexRange(geo_data.size), 256, [&](const IndexRange range) {
          this->integrate_angular_velocities(sub_delta_time_,
                                             rotations.slice(range),
                                             rotations.slice(range),
                                             angular_velocities.slice(range),
                                             inv_inertias.slice(range),
                                             external_torques.slice(range));
        });
        geo_data.pin_position_mask.foreach_index([&](const int point_i) {
          const float3 &begin_pos = geo_data.pin_position_begin[point_i];
          const float3 &end_pos = geo_data.pin_position_end[point_i];
          const float3 pin_pos = math::interpolate(begin_pos, end_pos, substep.end_factor);
          positions[point_i] = pin_pos;
        });
        geo_data.pin_rotation_mask.foreach_index([&](const int point_i) {
          const math::Quaternion &begin_rot = geo_data.pin_rotation_begin[point_i];
          const math::Quaternion &end_rot = geo_data.pin_rotation_end[point_i];
          const math::Quaternion pin_rot = math::interpolate(
              begin_rot, end_rot, substep.end_factor);
          rotations[point_i] = pin_rot;
        });

        velocity_attr.finish();
        position_attr.finish();
      }
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
      const float3 &external_torque = torques[i];
      const float3 &inv_inertia = inv_inertias[i];
      if (math::is_zero(inv_inertia)) {
        continue;
      }
      float3 &angular_velocity = angular_velocities[i];
      const float3 precession = math::cross(angular_velocity,
                                            math::safe_divide(angular_velocity, inv_inertia));
      angular_velocity += delta_time * (external_torque - precession) * inv_inertia;
      const math::Quaternion &old_rotation = old_rotations[i];
      const math::Quaternion direction = old_rotation * math::Quaternion(0, angular_velocity);
      new_rotations[i] = math::normalize(
          math::Quaternion(float4(old_rotation) + delta_time * 0.5f * float4(direction)));
    }
  }

  void write_back_geometries_to_world()
  {
    for (const int geo_bundle_i : geometries_.paths.index_range()) {
      const StringRef geometry_path = geometries_.paths[geo_bundle_i];
      GeometrySet &geometry = geometries_.geometry_sets[geo_bundle_i];
      world_.add_path_override(geometry_path + "/geometry", std::move(geometry));
    }
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
  const float delta_time = std::max(0.0f, params.get_input<float>("Delta Time"));
  Bundle &world = world_ptr.ensure_mutable_inplace();
  ResourceScope scope;

  XpbdSolverStep step(scope, world, delta_time, substeps);
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
