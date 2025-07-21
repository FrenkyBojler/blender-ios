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

static std::optional<Bundle::Item> lookup_bundle_path(const Bundle &bundle, const StringRef path)
{
  BLI_assert(!path.is_empty());
  BLI_assert(!path.endswith("/"));
  const int sep = path.find_first_of('/');
  const StringRef first_part = sep == StringRef::not_found ? path : path.substr(0, sep);
  const std::optional<Bundle::Item> item = bundle.lookup(SocketInterfaceKey{first_part});
  if (!item) {
    return std::nullopt;
  }
  if (first_part.size() == path.size()) {
    return item;
  }
  if (item->type->type != SOCK_BUNDLE) {
    return std::nullopt;
  }
  const BundlePtr child_bundle =
      static_cast<const bke::SocketValueVariant *>(item->value)->get<BundlePtr>();
  if (!child_bundle) {
    return std::nullopt;
  }
  return lookup_bundle_path(*child_bundle, path.substr(sep + 1));
}

static void store_bundle_path(Bundle &bundle,
                              const StringRef path,
                              const bke::bNodeSocketType &type,
                              const void *value)
{
  BLI_assert(!path.is_empty());
  BLI_assert(!path.endswith("/"));
  BLI_assert(bundle.is_mutable());
  const int sep = path.find_first_of('/');
  if (sep == StringRef::not_found) {
    bundle.remove(SocketInterfaceKey{path});
    bundle.add_new(SocketInterfaceKey{path}, type, value);
    return;
  }
  const StringRef first_part = path.substr(0, sep);
  BundlePtr child_bundle;
  const std::optional<Bundle::Item> item = bundle.lookup(SocketInterfaceKey{first_part});
  if (item && item->type->type == SOCK_BUNDLE) {
    child_bundle = static_cast<const bke::SocketValueVariant *>(item->value)->get<BundlePtr>();
  }
  else {
    child_bundle = Bundle::create();
  }
  bundle.remove(SocketInterfaceKey{path});
  if (!child_bundle->is_mutable()) {
    child_bundle = child_bundle->copy();
  }
  child_bundle->tag_ensured_mutable();
  store_bundle_path(const_cast<Bundle &>(*child_bundle), path.substr(sep + 1), type, value);
  bke::SocketValueVariant child_bundle_value = bke::SocketValueVariant::From(
      std::move(child_bundle));
  bundle.add(SocketInterfaceKey{first_part},
             *bke::node_socket_type_find_static(SOCK_BUNDLE),
             &child_bundle_value);
}

static void foreach_behavior_recursive(
    const Bundle &behaviors_bundle,
    Vector<StringRef> &path_stack,
    const FunctionRef<void(StringRef type, const Bundle &behavior_bundle, Span<StringRef> path)>
        fn)
{
  if (std::optional<const Bundle::Item> type_item = behaviors_bundle.lookup(
          SocketInterfaceKey{"Type"}))
  {
    if (type_item->type->type != SOCK_STRING) {
      return;
    }
    const std::string type =
        static_cast<const bke::SocketValueVariant *>(type_item->value)->get<std::string>();
    if (type.empty()) {
      return;
    }
    fn(type, behaviors_bundle, path_stack);
    return;
  }
  for (const Bundle::StoredItem &item : behaviors_bundle.items()) {
    if (item.type->type != SOCK_BUNDLE) {
      continue;
    }
    BundlePtr child_bundle = static_cast<bke::SocketValueVariant *>(item.value)->get<BundlePtr>();
    if (!child_bundle) {
      continue;
    }
    const StringRef key = item.key.identifiers()[0];
    path_stack.append(key);
    foreach_behavior_recursive(*child_bundle, path_stack, fn);
    path_stack.pop_last();
  }
}

template<typename T>
static std::optional<T> get_from_bundle__value_variant(const Bundle &bundle, const StringRef key)
{
  std::optional<Bundle::Item> item = bundle.lookup(SocketInterfaceKey{key});
  if (!item) {
    return std::nullopt;
  }
  if (item->type->geometry_nodes_cpp_type != &CPPType::get<bke::SocketValueVariant>()) {
    return std::nullopt;
  }
  if constexpr (fn::is_field_v<T>) {
    if (item->type->base_cpp_type != &CPPType::get<typename T::base_type>()) {
      return std::nullopt;
    }
  }
  else if (item->type->base_cpp_type != &CPPType::get<T>()) {
    return std::nullopt;
  }
  return static_cast<const bke::SocketValueVariant *>(item->value)->get<T>();
}

static geometry::xpbd::Behaviors parse_behaviors(const BundlePtr &behaviors_bundle,
                                                 ResourceScope &scope)
{
  if (!behaviors_bundle) {
    return {};
  }
  geometry::xpbd::Behaviors behaviors;
  Vector<StringRef> path_stack;
  foreach_behavior_recursive(
      *behaviors_bundle,
      path_stack,
      [&](const StringRef type, const Bundle &behavior_bundle, const Span<StringRef> path_stack) {
        const std::string path = combine_bundle_path(path_stack);
        if (type == "Geometry") {
          std::optional<Bundle::Item> geometry_item = behavior_bundle.lookup(
              SocketInterfaceKey{"Geometry"});
          if (!geometry_item) {
            return;
          }
          if (geometry_item->type->type != SOCK_GEOMETRY) {
            return;
          }
          geometry::xpbd::SimGeometrySet geometry;
          geometry.mass_attribute = "mass";
          geometry.velocity_attribute = "velocity";
          geometry.path = path;
          geometry.geometry = *static_cast<const GeometrySet *>(geometry_item->value);
          if (std::optional<std::string> mass_attribute =
                  get_from_bundle__value_variant<std::string>(behavior_bundle, "Mass Attribute"))
          {
            geometry.mass_attribute = *mass_attribute;
          }
          if (std::optional<std::string> velocity_item =
                  get_from_bundle__value_variant<std::string>(behavior_bundle,
                                                              "Velocity Attribute"))
          {
            geometry.velocity_attribute = *velocity_item;
          }
          behaviors.sim_geometry_sets.append(geometry);
          return;
        }
        if (type == "Force") {
          std::optional<Field<float3>> force_field = get_from_bundle__value_variant<Field<float3>>(
              behavior_bundle, "Force Field");
          if (!force_field) {
            return;
          }
          geometry::xpbd::ForceField force;
          force.force_field = *force_field;
          behaviors.force_fields.append(force);
          return;
        }
        if (type == "Acceleration") {
          std::optional<Field<float3>> acceleration_field =
              get_from_bundle__value_variant<Field<float3>>(behavior_bundle, "Acceleration Field");
          if (!acceleration_field) {
            return;
          }
          geometry::xpbd::AccelerationField acceleration;
          acceleration.acceleration_field = *acceleration_field;
          behaviors.acceleration_fields.append(acceleration);
          return;
        }
        if (type == "Edge Length Constraint") {
          std::optional<std::string> rest_length_attribute =
              get_from_bundle__value_variant<std::string>(behavior_bundle,
                                                          "Rest Length Attribute");
          if (!rest_length_attribute) {
            return;
          }
          if (rest_length_attribute->empty()) {
            return;
          }
          const float compliance =
              get_from_bundle__value_variant<float>(behavior_bundle, "Compliance").value_or(0.0f);
          behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__edge_lengths(
              scope, std::move(*rest_length_attribute), compliance));
          return;
        }
        if (type == "Curve Length Constraint") {
          std::optional<std::string> rest_length_attribute =
              get_from_bundle__value_variant<std::string>(behavior_bundle,
                                                          "Rest Length Attribute");
          if (!rest_length_attribute) {
            return;
          }
          if (rest_length_attribute->empty()) {
            return;
          }
          const float compliance =
              get_from_bundle__value_variant<float>(behavior_bundle, "Compliance").value_or(0.0f);
          behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__curve_lengths(
              scope, std::move(*rest_length_attribute), compliance));
          return;
        }
        if (type == "Fixed Position Constraint") {
          std::optional<Field<bool>> selection_field = get_from_bundle__value_variant<Field<bool>>(
              behavior_bundle, "Selection");
          std::optional<Field<float3>> positions_field =
              get_from_bundle__value_variant<Field<float3>>(behavior_bundle, "Position");
          if (!selection_field || !positions_field) {
            return;
          }
          behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__fixed_positions(
              scope, *selection_field, *positions_field));
          return;
        }
        if (type == "Infinite Collision Plane") {
          std::optional<float3> position = get_from_bundle__value_variant<float3>(behavior_bundle,
                                                                                  "Position");
          std::optional<float3> normal = get_from_bundle__value_variant<float3>(behavior_bundle,
                                                                                "Normal");
          if (!position || !normal) {
            return;
          }
          behaviors.constraint_sets.append(
              &geometry::xpbd::create_constraint__infinite_collision_plane(
                  scope, *position, *normal));
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
    for (geometry::xpbd::SimGeometrySet &sim_geometry : behaviors.sim_geometry_sets) {
      std::optional<Bundle::Item> item = lookup_bundle_path(*old_data_bundle,
                                                            sim_geometry.path + "/Geometry");
      if (!item) {
        continue;
      }
      if (item->type->type != SOCK_GEOMETRY) {
        continue;
      }
      sim_geometry.geometry = *static_cast<const GeometrySet *>(item->value);
    }
  }

  geometry::xpbd::solve(behaviors, delta_time, substeps);

  BundlePtr new_data_bundle = Bundle::create();
  for (geometry::xpbd::SimGeometrySet &sim_geometry : behaviors.sim_geometry_sets) {
    store_bundle_path(const_cast<Bundle &>(*new_data_bundle),
                      sim_geometry.path + "/Geometry",
                      *bke::node_socket_type_find_static(SOCK_GEOMETRY),
                      &sim_geometry.geometry);
  }

  params.set_output("Data", new_data_bundle);
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
