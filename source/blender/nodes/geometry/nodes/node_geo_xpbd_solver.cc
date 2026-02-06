/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"

#include "BKE_curves.hh"
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

  Vector<std::string> geometry_paths = gather_bundle_paths_by_type(world,
                                                                   XPBDGeometryBundle::name);

  Vector<GeometrySet> geometries(geometry_paths.size());
  for (const int i : geometry_paths.index_range()) {
    const StringRef geometry_path = geometry_paths[i];
    if (GeometrySet *geometry = world.lookup_path_for_write_ptr<GeometrySet>(geometry_path +
                                                                             "/geometry"))
    {
      geometries[i] = std::move(*geometry);
    }
  }

  Vector<bke::MutableAttributeAccessor> attribute_accessors;
  for (GeometrySet &geometry : geometries) {
    for (bke::GeometryComponent::Type type : {bke::GeometryComponent::Type::Mesh,
                                              bke::GeometryComponent::Type::PointCloud,
                                              bke::GeometryComponent::Type::Curve,
                                              bke::GeometryComponent::Type::Instance})
    {
      if (!geometry.has(type)) {
        continue;
      }
      bke::GeometryComponent &component = geometry.get_component_for_write(type);
      attribute_accessors.append(*component.attributes_for_write());
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
        attribute_accessors.append(curves.attributes_for_write());
      }
    }
  }

  for (const int accessor_i : attribute_accessors.index_range()) {
    bke::MutableAttributeAccessor &attributes = attribute_accessors[accessor_i];
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
        velocities[i] += acceleration * delta_time;
        positions[i] += velocities[i] * delta_time;
      }
    });
    velocities_attr.finish();
    positions_attr.finish();
  }

  for (const int i : geometry_paths.index_range()) {
    const StringRef geometry_path = geometry_paths[i];
    GeometrySet &geometry = geometries[i];
    world.add_path_override(geometry_path + "/geometry", std::move(geometry));
  }

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
