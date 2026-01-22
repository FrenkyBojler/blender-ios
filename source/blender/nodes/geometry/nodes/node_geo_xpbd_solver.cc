/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"

#include "BKE_curves.hh"

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
  // Bundle &solver_data = world.ensure_nested_bundle(solver_path);

  Vector<XPBDGeometryBundle> geometry_bundles;

  float3 gravity{};
  nested_bundle_foreach(world, [&](HandleNestedBundleParams &p) {
    if (p.type == XPBDGeometryBundle::name) {
      BundleParseErrors errors;
      if (std::optional<XPBDGeometryBundle> geometry_bundle = XPBDGeometryBundle::parse(p.bundle,
                                                                                        errors))
      {
        geometry_bundle->self_path = Bundle::combine_path(p.path);
        geometry_bundles.append(std::move(*geometry_bundle));
      }
    }
    if (p.type == GravityBundle::name) {
      BundleParseErrors errors;
      if (std::optional<GravityBundle> gravity_bundle = GravityBundle::parse(p.bundle, errors)) {
        gravity += gravity_bundle->gravity;
      }
    }
  });

  for (XPBDGeometryBundle &geometry_bundle : geometry_bundles) {
    if (!geometry_bundle.geometry.has_curves()) {
      continue;
    }
    Curves &curves_id = *geometry_bundle.geometry.get_curves_for_write();
    bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    bke::SpanAttributeWriter<float3> velocities =
        curves.attributes_for_write().lookup_or_add_for_write_span<float3>("velocity",
                                                                           AttrDomain::Point);
    bke::SpanAttributeWriter<float3> positions =
        curves.attributes_for_write().lookup_or_add_for_write_span<float3>("position",
                                                                           AttrDomain::Point);

    for (const int i : curves.points_range()) {
      velocities.span[i] += gravity * delta_time;
      positions.span[i] += velocities.span[i] * delta_time;
    }
    curves.tag_positions_changed();
    velocities.finish();
    positions.finish();
    world.add_path_override(geometry_bundle.self_path + "/geometry",
                            std::move(geometry_bundle.geometry));
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
