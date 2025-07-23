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

static geometry::xpbd::Behaviors parse_behaviors(const BundlePtr &behaviors_bundle,
                                                 ResourceScope &scope)
{
  if (!behaviors_bundle) {
    return {};
  }
  geometry::xpbd::Behaviors behaviors;
  foreach_behavior_in_bundle(
      *behaviors_bundle,
      [&](const StringRef type, const Bundle &behavior_bundle, const Span<StringRef> path_stack) {
        const std::string path = combine_bundle_path(path_stack);
        if (type == "Geometry") {
          std::optional<GeometrySet> geometry = behavior_bundle.lookup<GeometrySet>(
              SocketInterfaceKey{"Geometry"});
          if (!geometry) {
            return;
          }
          auto &sim_geometry_set = scope.construct<geometry::xpbd::SimGeometrySet>();
          sim_geometry_set.path = path;
          sim_geometry_set.geometry = *geometry;
          sim_geometry_set.mass_attribute = behavior_bundle
                                                .lookup<std::string>(
                                                    SocketInterfaceKey{"Mass Attribute"})
                                                .value_or("mass");
          sim_geometry_set.velocity_attribute = behavior_bundle
                                                    .lookup<std::string>(
                                                        SocketInterfaceKey{"Velocity"})
                                                    .value_or("velocity");
          behaviors.sim_geometry_sets.append(&sim_geometry_set);
          return;
        }
        if (type == "Force") {
          std::optional<Field<float3>> force_field = behavior_bundle.lookup<Field<float3>>(
              SocketInterfaceKey{"Force Field"});
          if (!force_field) {
            return;
          }
          geometry::xpbd::ForceField force;
          force.force_field = *force_field;
          behaviors.force_fields.append(force);
          return;
        }
        if (type == "Acceleration") {
          std::optional<Field<float3>> acceleration_field = behavior_bundle.lookup<Field<float3>>(
              SocketInterfaceKey{"Acceleration Field"});
          if (!acceleration_field) {
            return;
          }
          geometry::xpbd::AccelerationField acceleration;
          acceleration.acceleration_field = *acceleration_field;
          behaviors.acceleration_fields.append(acceleration);
          return;
        }
        if (type == "Edge Length Constraint") {
          std::optional<std::string> rest_length_attribute = behavior_bundle.lookup<std::string>(
              SocketInterfaceKey{"Rest Length Attribute"});
          if (!rest_length_attribute) {
            return;
          }
          if (rest_length_attribute->empty()) {
            return;
          }
          const float compliance =
              behavior_bundle.lookup<float>(SocketInterfaceKey{"Compliance"}).value_or(0.0f);
          behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__edge_lengths(
              scope, std::move(*rest_length_attribute), compliance));
          return;
        }
        if (type == "Curve Length Constraint") {
          std::optional<std::string> rest_length_attribute = behavior_bundle.lookup<std::string>(
              SocketInterfaceKey{"Rest Length Attribute"});
          if (!rest_length_attribute) {
            return;
          }
          if (rest_length_attribute->empty()) {
            return;
          }
          const float compliance =
              behavior_bundle.lookup<float>(SocketInterfaceKey{"Compliance"}).value_or(0.0f);
          behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__curve_lengths(
              scope, std::move(*rest_length_attribute), compliance));
          return;
        }
        if (type == "Fixed Position Constraint") {
          std::optional<Field<bool>> selection_field = behavior_bundle.lookup<Field<bool>>(
              SocketInterfaceKey{"Selection"});
          std::optional<Field<float3>> positions_field = behavior_bundle.lookup<Field<float3>>(
              SocketInterfaceKey{"Position"});
          if (!selection_field || !positions_field) {
            return;
          }
          behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__fixed_positions(
              scope, *selection_field, *positions_field));
          return;
        }
        if (type == "Infinite Collision Plane") {
          std::optional<float3> position = behavior_bundle.lookup<float3>(
              SocketInterfaceKey{"Position"});
          std::optional<float3> normal = behavior_bundle.lookup<float3>(
              SocketInterfaceKey{"Normal"});
          if (!position || !normal) {
            return;
          }
          behaviors.constraint_sets.append(
              &geometry::xpbd::create_constraint__infinite_collision_plane(
                  scope, *position, *normal));
          return;
        }
        if (type == "Global Volume Constraint") {
          std::optional<std::string> rest_volume_name = behavior_bundle.lookup<std::string>(
              SocketInterfaceKey{"Rest Volume Name"});
          if (!rest_volume_name || rest_volume_name->empty()) {
            return;
          }
          const float overpressure =
              behavior_bundle.lookup<float>(SocketInterfaceKey{"Overpressure"}).value_or(1.0f);
          behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__global_volume(
              scope, std::move(*rest_volume_name), overpressure));
          return;
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
      if (std::optional<GeometrySet> geometry = item.lookup<GeometrySet>(
              SocketInterfaceKey{"Geometry"}))
      {
        sim_geometry->geometry = std::move(*geometry);
      }
      if (std::optional<BundlePtr> extra = item.lookup<BundlePtr>(SocketInterfaceKey{"Extra"})) {
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
