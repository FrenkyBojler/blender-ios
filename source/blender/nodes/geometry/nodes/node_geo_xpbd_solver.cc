/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "BKE_curves.hh"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "GEO_xpbd.hh"

#include "node_geometry_util.hh"

#include "NOD_geometry_nodes_behaviors_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"

namespace blender::nodes::node_geo_xpbd_solver_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bundle>("Data");
  b.add_output<decl::Bundle>("Data").align_with_previous();
  b.add_input<decl::Bundle>("Behavior");
  b.add_input<decl::Float>("Delta Time").min(0).hide_value();
  b.add_input<decl::Int>("Substeps").default_value(0).min(0);
}

static std::string combine_bundle_path(const Span<StringRef> &path)
{
  return fmt::format("{}", fmt::join(path, "/"));
}

struct ParseBehaviorParams {
  const Span<StringRef> path_elems;
  const Bundle &bundle;
  ResourceScope &scope;
  geometry::xpbd::Behaviors &r_behaviors;
};

static void parse_behavior__geometry(ParseBehaviorParams &params)
{
  std::optional<GeometrySet> geometry = params.bundle.lookup<GeometrySet>("Geometry");
  if (!geometry) {
    return;
  }
  auto &sim_geometry_set = params.scope.construct<geometry::xpbd::SimGeometrySet>();
  sim_geometry_set.path = combine_bundle_path(params.path_elems);
  sim_geometry_set.geometry = *geometry;
  sim_geometry_set.mass_attribute =
      params.bundle.lookup<std::string>("Mass Attribute").value_or("mass");
  sim_geometry_set.velocity_attribute =
      params.bundle.lookup<std::string>("Velocity").value_or("velocity");
  params.r_behaviors.sim_geometry_sets.append(&sim_geometry_set);
}

static void parse_behavior__force(ParseBehaviorParams &params)
{
  std::optional<Field<float3>> force_field = params.bundle.lookup<Field<float3>>("Force Field");
  if (!force_field) {
    return;
  }
  geometry::xpbd::ForceField force;
  force.force_field = *force_field;
  params.r_behaviors.force_fields.append(force);
}

static void parse_behavior__acceleration(ParseBehaviorParams &params)
{
  std::optional<Field<float3>> acceleration_field = params.bundle.lookup<Field<float3>>(
      "Acceleration Field");
  if (!acceleration_field) {
    return;
  }
  geometry::xpbd::AccelerationField acceleration;
  acceleration.acceleration_field = *acceleration_field;
  params.r_behaviors.acceleration_fields.append(acceleration);
}

static void parse_behavior__edge_lengths(ParseBehaviorParams &params)
{
  std::optional<std::string> rest_length_attribute = params.bundle.lookup<std::string>(
      "Rest Length Attribute");
  if (!rest_length_attribute) {
    return;
  }
  if (rest_length_attribute->empty()) {
    return;
  }
  const float compliance = params.bundle.lookup<float>("Compliance").value_or(0.0f);
  params.r_behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__edge_lengths(
      params.scope, std::move(*rest_length_attribute), compliance));
}

static void parse_behavior__curve_lengths(ParseBehaviorParams &params)
{
  std::optional<std::string> rest_length_attribute = params.bundle.lookup<std::string>(
      "Rest Length Attribute");
  if (!rest_length_attribute) {
    return;
  }
  if (rest_length_attribute->empty()) {
    return;
  }
  const float compliance = params.bundle.lookup<float>("Compliance").value_or(0.0f);
  params.r_behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__curve_lengths(
      params.scope, std::move(*rest_length_attribute), compliance));
}

static void parse_behavior__fixed_positions(ParseBehaviorParams &params)
{
  std::optional<Field<bool>> selection_field = params.bundle.lookup<Field<bool>>("Selection");
  std::optional<Field<float3>> positions_field = params.bundle.lookup<Field<float3>>("Position");
  if (!selection_field || !positions_field) {
    return;
  }
  params.r_behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__fixed_positions(
      params.scope, *selection_field, *positions_field));
}

static void parse_behavior__infinite_collision_plane(ParseBehaviorParams &params)
{
  std::optional<float3> position = params.bundle.lookup<float3>("Position");
  std::optional<float3> normal = params.bundle.lookup<float3>("Normal");
  if (!position || !normal) {
    return;
  }
  params.r_behaviors.constraint_sets.append(
      &geometry::xpbd::create_constraint__infinite_collision_plane(
          params.scope, *position, *normal));
}

static void parse_behavior__global_volume(ParseBehaviorParams &params)
{
  std::optional<std::string> rest_volume_name = params.bundle.lookup<std::string>(
      "Rest Volume Name");
  if (!rest_volume_name || rest_volume_name->empty()) {
    return;
  }
  const float overpressure = params.bundle.lookup<float>("Overpressure").value_or(1.0f);
  params.r_behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__global_volume(
      params.scope, std::move(*rest_volume_name), overpressure));
}

using BehaviorParserFn = std::function<void(ParseBehaviorParams &)>;

static Map<std::string, BehaviorParserFn> build_behavior_parsers()
{
  Map<std::string, BehaviorParserFn> behavior_parsers;
  behavior_parsers.add_new("Geometry", parse_behavior__geometry);
  behavior_parsers.add_new("Force", parse_behavior__force);
  behavior_parsers.add_new("Acceleration", parse_behavior__acceleration);
  behavior_parsers.add_new("Edge Length Constraint", parse_behavior__edge_lengths);
  behavior_parsers.add_new("Curve Length Constraint", parse_behavior__curve_lengths);
  behavior_parsers.add_new("Fixed Position Constraint", parse_behavior__fixed_positions);
  behavior_parsers.add_new("Infinite Collision Plane", parse_behavior__infinite_collision_plane);
  behavior_parsers.add_new("Global Volume Constraint", parse_behavior__global_volume);
  return behavior_parsers;
}

static geometry::xpbd::Behaviors parse_behaviors(const BundlePtr &behaviors_bundle,
                                                 ResourceScope &scope)
{
  if (!behaviors_bundle) {
    return {};
  }
  geometry::xpbd::Behaviors behaviors;
  static const Map<std::string, BehaviorParserFn> behavior_parsers = build_behavior_parsers();
  foreach_behavior_in_bundle(
      *behaviors_bundle,
      [&](const StringRef type, const Bundle &behavior_bundle, const Span<StringRef> path_stack) {
        ParseBehaviorParams params{path_stack, behavior_bundle, scope, behaviors};
        if (const auto *parser = behavior_parsers.lookup_ptr(type)) {
          (*parser)(params);
        }
      });
  return behaviors;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr old_data_bundle = params.extract_input<BundlePtr>("Data");
  BundlePtr behaviors_bundle = params.extract_input<BundlePtr>("Behavior");
  const float delta_time = std::max(0.0f, params.extract_input<float>("Delta Time"));
  const int substeps = std::max(0, params.extract_input<int>("Substeps"));

  ResourceScope scope;
  geometry::xpbd::Behaviors behaviors = parse_behaviors(behaviors_bundle, scope);
  if (old_data_bundle) {
    for (geometry::xpbd::SimGeometrySet *sim_geometry : behaviors.sim_geometry_sets) {
      std::optional<BundlePtr> item_ptr = old_data_bundle->lookup_path<BundlePtr>(
          sim_geometry->path);
      if (!item_ptr || !*item_ptr) {
        continue;
      }
      const Bundle &item = **item_ptr;
      if (std::optional<GeometrySet> geometry = item.lookup<GeometrySet>("Geometry")) {
        sim_geometry->geometry = std::move(*geometry);
      }
      if (std::optional<BundlePtr> extra = item.lookup<BundlePtr>("Extra")) {
        sim_geometry->extra = std::move(*extra);
      }
    }
  }

  geometry::xpbd::solve(behaviors, delta_time, substeps);

  BundlePtr new_data_bundle_ptr = Bundle::create();
  Bundle &new_data_bundle = const_cast<Bundle &>(*new_data_bundle_ptr);
  for (geometry::xpbd::SimGeometrySet *sim_geometry : behaviors.sim_geometry_sets) {
    new_data_bundle.add_path_override(sim_geometry->path + "/Geometry", sim_geometry->geometry);
    if (sim_geometry->extra && !sim_geometry->extra->items().is_empty()) {
      new_data_bundle.add_path_override(sim_geometry->path + "/Extra", sim_geometry->extra);
    }
  }

  params.set_output("Data", new_data_bundle_ptr);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeXPBDSolver");
  ntype.ui_name = "XPBD Solver";
  ntype.ui_description = "Solve geometry constraints using the XPBD solver framework";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_xpbd_solver_cc
