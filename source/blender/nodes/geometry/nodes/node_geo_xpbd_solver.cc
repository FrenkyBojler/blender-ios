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

#include "NOD_geometry_nodes_behaviors.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"

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

struct ParseBehaviorParams {
  const Span<StringRef> path_elems;
  const Bundle &bundle;
  ResourceScope &scope;
  geometry::xpbd::Behaviors &r_behaviors;

  std::string self_path() const
  {
    return Bundle::combine_path(this->path_elems);
  }
};

static void parse_behavior__geometry(ParseBehaviorParams &params)
{
  std::optional<GeometrySet> geometry = params.bundle.lookup<GeometrySet>("Geometry");
  if (!geometry) {
    return;
  }
  auto &sim_geometry_set = params.scope.construct<geometry::xpbd::SimGeometrySet>();
  sim_geometry_set.path = params.self_path();
  sim_geometry_set.geometry = *geometry;
  sim_geometry_set.mass_attribute =
      params.bundle.lookup<std::string>("Mass Attribute").value_or("mass");
  sim_geometry_set.inertia_attribute =
      params.bundle.lookup<std::string>("Inertia Attribute").value_or("inertia");
  sim_geometry_set.rotation_attribute =
      params.bundle.lookup<std::string>("Rotation").value_or("rotation");
  sim_geometry_set.velocity_attribute =
      params.bundle.lookup<std::string>("Velocity").value_or("velocity");
  sim_geometry_set.angular_velocity_attribute =
      params.bundle.lookup<std::string>("Angular Velocity").value_or("angular_velocity");
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
  force.self_path = params.self_path();
  force.filter = params.bundle.lookup<std::string>("Filter").value_or("");
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
  acceleration.self_path = params.self_path();
  acceleration.filter = params.bundle.lookup<std::string>("Filter").value_or("");
  params.r_behaviors.acceleration_fields.append(acceleration);
}

static void parse_behavior__damping(ParseBehaviorParams &params)
{
  float linear_damping = params.bundle.lookup<float>("Linear Damping").value_or(0.0f);
  float angular_damping = params.bundle.lookup<float>("Angular Damping").value_or(0.0f);
  geometry::xpbd::Damping damping;
  damping.linear_damping = linear_damping;
  damping.angular_damping = angular_damping;
  damping.self_path = params.self_path();
  damping.filter = params.bundle.lookup<std::string>("Filter").value_or("");
  params.r_behaviors.dampings.append(damping);
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
  std::string filter = params.bundle.lookup<std::string>("Filter").value_or("");
  const float compliance = params.bundle.lookup<float>("Compliance").value_or(0.0f);
  params.r_behaviors.constraint_sets.append(
      &geometry::xpbd::create_constraint__edge_lengths(params.scope,
                                                       params.self_path(),
                                                       std::move(filter),
                                                       std::move(*rest_length_attribute),
                                                       compliance));
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
  std::string filter = params.bundle.lookup<std::string>("Filter").value_or("");
  const float compliance = params.bundle.lookup<float>("Compliance").value_or(0.0f);
  params.r_behaviors.constraint_sets.append(
      &geometry::xpbd::create_constraint__curve_lengths(params.scope,
                                                        params.self_path(),
                                                        std::move(filter),
                                                        std::move(*rest_length_attribute),
                                                        compliance));
}

static void parse_behavior__cosserat_rod_lengths(ParseBehaviorParams &params)
{
  std::optional<std::string> rest_length_attribute = params.bundle.lookup<std::string>(
      "Rest Length Attribute");
  if (!rest_length_attribute || rest_length_attribute->empty()) {
    return;
  }
  std::string filter = params.bundle.lookup<std::string>("Filter").value_or("");
  const float compliance = params.bundle.lookup<float>("Compliance").value_or(0.0f);
  params.r_behaviors.constraint_sets.append(
      &geometry::xpbd::create_constraint__cosserat_rod_lengths(params.scope,
                                                               params.self_path(),
                                                               std::move(filter),
                                                               std::move(*rest_length_attribute),
                                                               compliance));
}

static void parse_behavior__cosserat_rod_bending(ParseBehaviorParams &params)
{
  std::optional<std::string> rest_length_attribute = params.bundle.lookup<std::string>(
      "Rest Length Attribute");
  if (!rest_length_attribute || rest_length_attribute->empty()) {
    return;
  }
  std::optional<std::string> rest_shape_attribute = params.bundle.lookup<std::string>(
      "Rest Shape Attribute");
  if (!rest_shape_attribute || rest_shape_attribute->empty()) {
    return;
  }
  std::string filter = params.bundle.lookup<std::string>("Filter").value_or("");
  const float compliance = params.bundle.lookup<float>("Compliance").value_or(0.0f);
  params.r_behaviors.constraint_sets.append(
      &geometry::xpbd::create_constraint__cosserat_rod_bending(params.scope,
                                                               params.self_path(),
                                                               std::move(filter),
                                                               std::move(*rest_length_attribute),
                                                               std::move(*rest_shape_attribute),
                                                               compliance));
}

static void parse_behavior__fixed_positions(ParseBehaviorParams &params)
{
  std::optional<Field<bool>> selection_field = params.bundle.lookup<Field<bool>>("Selection");
  std::optional<Field<float3>> positions_field = params.bundle.lookup<Field<float3>>("Position");
  if (!selection_field || !positions_field) {
    return;
  }
  std::string filter = params.bundle.lookup<std::string>("Filter").value_or("");
  params.r_behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__fixed_positions(
      params.scope, params.self_path(), std::move(filter), *selection_field, *positions_field));
}

static void parse_behavior__fixed_rotations(ParseBehaviorParams &params)
{
  std::optional<Field<bool>> selection_field = params.bundle.lookup<Field<bool>>("Selection");
  std::optional<Field<math::Quaternion>> rotations_field =
      params.bundle.lookup<Field<math::Quaternion>>("Rotation");
  if (!selection_field || !rotations_field) {
    return;
  }
  std::string filter = params.bundle.lookup<std::string>("Filter").value_or("");
  const float compliance = params.bundle.lookup<float>("Compliance").value_or(0.0f);
  params.r_behaviors.constraint_sets.append(
      &geometry::xpbd::create_constraint__fixed_rotations(params.scope,
                                                          params.self_path(),
                                                          std::move(filter),
                                                          *selection_field,
                                                          *rotations_field,
                                                          compliance));
}

static void parse_behavior__infinite_collision_plane(ParseBehaviorParams &params)
{
  std::optional<float3> position = params.bundle.lookup<float3>("Position");
  std::optional<float3> normal = params.bundle.lookup<float3>("Normal");
  if (!position || !normal) {
    return;
  }
  std::string filter = params.bundle.lookup<std::string>("Filter").value_or("");
  params.r_behaviors.constraint_sets.append(
      &geometry::xpbd::create_constraint__infinite_collision_plane(
          params.scope, params.self_path(), std::move(filter), *position, *normal));
}

static void parse_behavior__global_volume(ParseBehaviorParams &params)
{
  std::optional<std::string> rest_volume_name = params.bundle.lookup<std::string>(
      "Rest Volume Name");
  if (!rest_volume_name || rest_volume_name->empty()) {
    return;
  }
  std::string filter = params.bundle.lookup<std::string>("Filter").value_or("");
  const float overpressure = params.bundle.lookup<float>("Overpressure").value_or(1.0f);
  params.r_behaviors.constraint_sets.append(
      &geometry::xpbd::create_constraint__global_volume(params.scope,
                                                        params.self_path(),
                                                        std::move(filter),
                                                        std::move(*rest_volume_name),
                                                        overpressure));
}

using BehaviorParserFn = std::function<void(ParseBehaviorParams &)>;

static Map<std::string, BehaviorParserFn> build_behavior_parsers()
{
  Map<std::string, BehaviorParserFn> behavior_parsers;
  behavior_parsers.add_new("Geometry", parse_behavior__geometry);
  behavior_parsers.add_new("Force", parse_behavior__force);
  behavior_parsers.add_new("Acceleration", parse_behavior__acceleration);
  behavior_parsers.add_new("Damping", parse_behavior__damping);
  behavior_parsers.add_new("Edge Length Constraint", parse_behavior__edge_lengths);
  behavior_parsers.add_new("Curve Length Constraint", parse_behavior__curve_lengths);
  behavior_parsers.add_new("Cosserat Rod Length Constraint", parse_behavior__cosserat_rod_lengths);
  behavior_parsers.add_new("Cosserat Rod Bending Constraint",
                           parse_behavior__cosserat_rod_bending);
  behavior_parsers.add_new("Fixed Position Constraint", parse_behavior__fixed_positions);
  behavior_parsers.add_new("Fixed Rotation Constraint", parse_behavior__fixed_rotations);
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
  nested_bundle_foreach(*behaviors_bundle, [&](HandleNestedBundleParams &params) {
    ParseBehaviorParams my_params{params.path, params.bundle, scope, behaviors};
    if (const auto *parser = behavior_parsers.lookup_ptr(params.type)) {
      (*parser)(my_params);
    }
  });
  return behaviors;
}

static void copy_attribute_data(const bke::AttributeAccessor src,
                                bke::MutableAttributeAccessor dst,
                                const StringRef name)
{
  const bke::GAttributeReader src_attribute = src.lookup(name);
  if (!src_attribute) {
    return;
  }
  if (src_attribute.varray.size() != dst.domain_size(src_attribute.domain)) {
    return;
  }
  const bke::AttrType attr_type = bke::cpp_type_to_attribute_type(src_attribute.varray.type());
  dst.remove(name);
  bke::GSpanAttributeWriter dst_attribute = dst.lookup_or_add_for_write_only_span(
      name, src_attribute.domain, attr_type);
  src_attribute.varray.materialize(dst_attribute.span.data());
  dst_attribute.finish();
}

static void copy_xpbd_simulated_attributes(
    const bke::AttributeAccessor src,
    bke::MutableAttributeAccessor dst,
    const geometry::xpbd::SimGeometrySet &sim_geometry_params)
{
  Vector<std::string> attributes_to_copy;
  attributes_to_copy.append("position");
  attributes_to_copy.append(sim_geometry_params.velocity_attribute);
  src.foreach_attribute([&](const AttributeIter &iter) {
    if (!dst.contains(iter.name)) {
      attributes_to_copy.append(iter.name);
    }
  });
  for (const StringRef name : attributes_to_copy) {
    copy_attribute_data(src, dst, name);
  }
}

static GeometrySet merge_behavior_with_sim_geometry(
    const GeometrySet &behavior_geometry,
    const GeometrySet &sim_geometry,
    const geometry::xpbd::SimGeometrySet &sim_geometry_params)
{
  GeometrySet merged_geometry = behavior_geometry;
  if (Mesh *mesh = merged_geometry.get_mesh_for_write()) {
    if (const Mesh *sim_mesh = sim_geometry.get_mesh()) {
      if (mesh->verts_num == sim_mesh->verts_num) {
        copy_xpbd_simulated_attributes(
            sim_mesh->attributes(), mesh->attributes_for_write(), sim_geometry_params);
      }
    }
  }
  if (Curves *curves_id = merged_geometry.get_curves_for_write()) {
    if (const Curves *sim_curves_id = sim_geometry.get_curves()) {
      bke::CurvesGeometry &curves = curves_id->geometry.wrap();
      const bke::CurvesGeometry &sim_curves = sim_curves_id->geometry.wrap();
      if (curves.points_num() == sim_curves.points_num()) {
        copy_xpbd_simulated_attributes(
            sim_curves.attributes(), curves.attributes_for_write(), sim_geometry_params);
      }
    }
  }
  if (PointCloud *pointcloud = merged_geometry.get_pointcloud_for_write()) {
    if (const PointCloud *sim_pointcloud = sim_geometry.get_pointcloud()) {
      if (pointcloud->totpoint == sim_pointcloud->totpoint) {
        copy_xpbd_simulated_attributes(
            sim_pointcloud->attributes(), pointcloud->attributes_for_write(), sim_geometry_params);
      }
    }
  }
  return merged_geometry;
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
      BundlePtr item =
          old_data_bundle->lookup_path<BundlePtr>(sim_geometry->path).value_or(nullptr);
      if (!item) {
        continue;
      }
      GeometrySet behavior_geometry = sim_geometry->geometry;
      GeometrySet old_sim_geometry = item->lookup<GeometrySet>("Geometry").value_or(GeometrySet());

      sim_geometry->geometry = merge_behavior_with_sim_geometry(
          behavior_geometry, old_sim_geometry, *sim_geometry);
      sim_geometry->extra = item->lookup<BundlePtr>("Extra").value_or(nullptr);
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
