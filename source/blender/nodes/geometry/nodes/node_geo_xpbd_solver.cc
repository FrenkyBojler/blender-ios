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

struct Geometries {
  Vector<std::string> paths;
  Vector<GeometrySet> geometry_sets;

  VectorSet<DataKey> data_keys;
  Vector<AttrDomain> domains;
  Vector<bke::MutableAttributeAccessor> attribute_accessors;

  /**
   * Inverse of the mass attribute + extra changes:
   * - For pinned positions this is set to 0.
   * - If there is no mass attribute, or it is <= 0, this is set to 1.
   */
  Vector<Vector<float>> inverse_masses_;
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

struct PinPositionConstraintInfo {
  int pin_position_bundle_i;
  int data_key_i;

  struct {
    fn::FieldEvaluator *evaluator = nullptr;
    int position_i;
    int compliance_terms_i;
  } eval;

  struct {
    std::string lambda;
  } attributes;

  Vector<int> indices;
  Vector<StartStopPair<float3>> animations;
  std::string lambda_attribute_name;
};

struct ConstraintsInfo {
  Vector<PinPositionConstraintInfo> pin_positions;
};

class XpbdSolverStep {
 private:
  ResourceScope &scope_;
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
    this->prepare_inverse_masses();
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
        geometries_.attribute_accessors.append(*component.attributes_for_write());
        geometries_.domains.append(type == bke::GeometryComponent::Type::Instance ?
                                       AttrDomain::Instance :
                                       AttrDomain::Point);
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
          geometries_.attribute_accessors.append(curves.attributes_for_write());
          geometries_.data_keys.add_new(
              {geo_bundle_i, bke::GeometryComponent::Type::Curve, layer_i});
          geometries_.domains.append(AttrDomain::Point);
        }
      }
    }
  }

  void prepare_inverse_masses()
  {
    geometries_.inverse_masses_.resize(geometries_.data_keys.size());
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      const AttrDomain domain = geometries_.domains[data_key_i];
      const bke::AttributeAccessor &attributes = geometries_.attribute_accessors[data_key_i];
      const int domain_size = attributes.domain_size(domain);
      Vector<float> &inverse_masses = geometries_.inverse_masses_[data_key_i];
      inverse_masses.resize(domain_size);
      const bke::AttributeReader<float> masses_attr = attributes.lookup<float>("mass", domain);
      const bke::AttributeReader<bool> pin_attr = attributes.lookup_or_default<bool>(
          "sim_pinned", domain, false);

      if (masses_attr) {
        threading::parallel_for(inverse_masses.index_range(), 2048, [&](const IndexRange range) {
          for (const int i : range) {
            const float mass = masses_attr.varray[i];
            const bool pinned = pin_attr.varray[i];
            if (pinned) {
              inverse_masses[i] = 0.0f;
            }
            else if (mass <= 0.0f) {
              inverse_masses[i] = 1.0f;
            }
            else {
              inverse_masses[i] = 1.0f / mass;
            }
          }
        });
      }
      else {
        threading::parallel_for(inverse_masses.index_range(), 2048, [&](const IndexRange range) {
          for (const int i : range) {
            const bool pinned = pin_attr.varray[i];
            if (pinned) {
              inverse_masses[i] = 0.0f;
            }
            else {
              inverse_masses[i] = 1.0f;
            }
          }
        });
      }
    }
  }

  void gather_constraints_from_world()
  {
    this->gather_constraints_from_world__pin_positions();
  }

  void gather_constraints_from_world__pin_positions()
  {
    const Vector<std::string> paths = gather_bundle_paths_by_type(
        world_, PinnedPositionXPBDConstraintBundle::name);
    for (const int bundle_i : paths.index_range()) {
      const StringRef path = paths[bundle_i];
      const Bundle &bundle = **world_.lookup_path_ptr<BundlePtr>(path);
      const std::optional<Field<float3>> position = bundle.lookup<Field<float3>>("position");
      if (!position) {
        continue;
      }
      const std::optional<Field<bool>> selection = bundle.lookup<Field<bool>>("selection");
      const std::string filter = bundle.lookup<std::string>("filter").value_or("");

      const Vector<int> data_keys = find_data_keys_for_filter(path, filter);
      for (const int data_key_i : data_keys) {
        PinPositionConstraintInfo info;
        info.data_key_i = data_key_i;
        info.pin_position_bundle_i = bundle_i;
        info.eval.evaluator = &this->get_field_evaluator(data_key_i, AttrDomain::Point, selection);
        info.eval.position_i = info.eval.evaluator->add(*position);
        info.eval.compliance_terms_i = info.eval.evaluator->add(
            this->get_compliance_term_field(bundle, "compliance"));
        constraints_info_.pin_positions.append(std::move(info));
      }
    }
  }

  void prepare_constraints__pin_positions()
  {
    for (PinPositionConstraintInfo &info : constraints_info_.pin_positions) {
      const IndexMask &mask = info.eval.evaluator->get_evaluated_selection_as_mask();
      const VArray<float3> pin_positions = info.eval.evaluator->get_evaluated<float3>(
          info.eval.position_i);
      const VArray<float> compliance_terms = info.eval.evaluator->get_evaluated<float>(
          info.eval.compliance_terms_i);

      const int pin_positions_num = mask.size();
      info.indices.resize(pin_positions_num);
      info.animations.resize(pin_positions_num);

      // const VArraySpan<float3> old_positions =
      //     *geometries_.attribute_accessors[info.data_key_i].lookup<float3>(
      //         "position", geometries_.domains[info.data_key_i]);

      mask.to_indices(info.indices.as_mutable_span());
      // for (const int i : IndexRange(pin_positions_num)) {
      //   const int point_i = info.indices[i];
      //   const float3 &old_position = old_positions[point_i];
      //   const float3 &pin_position = pin_positions[i];
      //   const float compliance_term = compliance_terms[i];
      // }
    }
  }

  fn::FieldEvaluator &get_field_evaluator(const int data_key_i,
                                          const AttrDomain domain,
                                          std::optional<Field<bool>> selection)
  {
    FieldEvaluatorKey key{data_key_i, domain, selection ? *selection : get_constant_true_field()};
    return *field_evaluators_.lookup_or_add_cb(key, [&]() {
      const auto &field_context = this->make_geometry_field_context(data_key_i, domain);
      const int domain_size = geometries_.attribute_accessors[data_key_i].domain_size(domain);
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

  void prepare_constraints()
  {
    this->prepare_constraints__pin_positions();
  }

  void do_simulation()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      bke::MutableAttributeAccessor &attributes = geometries_.attribute_accessors[data_key_i];
      bke::SpanAttributeWriter<float3> positions_attr =
          attributes.lookup_or_add_for_write_span<float3>("position", AttrDomain::Point);
      bke::SpanAttributeWriter<float3> velocities_attr =
          attributes.lookup_or_add_for_write_span<float3>("velocity", AttrDomain::Point);
      MutableSpan<float3> positions = positions_attr.span;
      MutableSpan<float3> velocities = velocities_attr.span;
      const VArraySpan<float3> external_forces = *attributes.lookup_or_default<float3>(
          "external_force", AttrDomain::Point, float3(0, 0, 0));
      const Span<float> inv_masses = geometries_.inverse_masses_[data_key_i];
      threading::parallel_for(positions.index_range(), 256, [&](const IndexRange range) {
        for (const int i : range) {
          const float3 external_force = external_forces[i];
          const float inv_mass = inv_masses[i];
          const float3 acceleration = external_force * inv_mass;
          velocities[i] += acceleration * total_delta_time_;
          positions[i] += velocities[i] * total_delta_time_;
        }
      });
      velocities_attr.finish();
      positions_attr.finish();
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
