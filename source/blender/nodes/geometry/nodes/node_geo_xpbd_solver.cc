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
  panel.add_input<decl::String>("Path").default_value("solvers/xpbd");
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
  Vector<bke::MutableAttributeAccessor> attribute_accessors;
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

/** Utility to gather many field evaluations to be able to do them all at once. */
class FieldEvaluators {
 private:
  ResourceScope &scope_;
  Map<FieldEvaluatorKey, fn::FieldEvaluator *> evaluators_;

 public:
  FieldEvaluators(ResourceScope &scope) : scope_(scope) {}

  fn::FieldEvaluator &ensure(const int data_key_i,
                             const bke::GeometryComponent &component,
                             const AttrDomain domain,
                             const std::optional<Field<bool>> selection)
  {
    FieldEvaluatorKey key{data_key_i, domain, selection ? *selection : get_constant_true_field()};
    return *evaluators_.lookup_or_add_cb(key, [&]() {
      auto &field_context = scope_.construct<bke::GeometryFieldContext>(component, domain);
      const int domain_size = component.attribute_domain_size(domain);
      auto &evaluator = scope_.construct<fn::FieldEvaluator>(field_context, domain_size);
      if (selection) {
        evaluator.set_selection(*selection);
      }
      return &evaluator;
    });
  }
};

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
    const fn::FieldEvaluator *evaluator = nullptr;
    int position_i;
    int compliance_terms_i;
  } eval;

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
  const float delta_time_;
  FieldEvaluators field_evaluators_;

  Geometries geometries_;
  ConstraintsInfo constraints_info_;

 public:
  XpbdSolverStep(ResourceScope &scope, Bundle &world, const float delta_time)
      : scope_(scope), world_(world), delta_time_(delta_time), field_evaluators_(scope_)
  {
  }

  void do_step()
  {
    this->gather_geometries_from_world();
    this->gather_constraints_from_world();
    this->do_simulation();
    this->write_back_geometries_to_world();
  }

 private:
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
        }
      }
    }
  }

  void gather_constraints_from_world()
  {
    const Vector<std::string> pin_positions_bundle_paths = gather_bundle_paths_by_type(
        world_, PinnedPositionXPBDConstraintBundle::name);
    for (const int pin_position_bundle_i : pin_positions_bundle_paths.index_range()) {
      const StringRef pin_positions_path = pin_positions_bundle_paths[pin_position_bundle_i];
    }
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
      const VArraySpan<float> masses = *attributes.lookup_or_default<float>(
          "mass", AttrDomain::Point, 1);
      threading::parallel_for(positions.index_range(), 256, [&](const IndexRange range) {
        for (const int i : range) {
          const float3 external_force = external_forces[i];
          const float mass = masses[i];
          const float3 acceleration = math::safe_divide(external_force, mass);
          velocities[i] += acceleration * delta_time_;
          positions[i] += velocities[i] * delta_time_;
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
  const std::string solver_path = params.get_input<std::string>("Path");
  if (!Bundle::is_valid_path(solver_path)) {
    params.set_output("World", std::move(world_ptr));
    return;
  }
  const float delta_time = std::max(0.0f, params.get_input<float>("Delta Time"));
  Bundle &world = world_ptr.ensure_mutable_inplace();
  ResourceScope scope;

  XpbdSolverStep step(scope, world, delta_time);
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
