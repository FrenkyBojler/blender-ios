/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "BKE_curves.hh"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"
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

class LocalXpbdConstraintCorrections {
 public:
  void add_position_correction(const int geometry_i, const int vertex_i, const float3 &correction);
};

class XpbdConstraintCorrections {
  LocalXpbdConstraintCorrections &local();
};

class XpbdContraints {
 public:
  virtual void solve(const XpbdConstraintCorrections &corrections) = 0;
};

class SimForce {
 public:
  Field<float3> force_field;
};

class SimAcceleration {
 public:
  Field<float3> acceleration_field;
};

struct SimGeometrySet {
  std::string path;
  GeometrySet geometry;
};

struct SimGeometry {
  std::string path;
  std::variant<Mesh *, PointCloud *, Curves *> data;

  std::optional<bke::MutableAttributeAccessor> attributes_for_write()
  {
    if (Mesh **mesh = std::get_if<Mesh *>(&data)) {
      return (*mesh)->attributes_for_write();
    }
    if (PointCloud **pointcloud = std::get_if<PointCloud *>(&data)) {
      return (*pointcloud)->attributes_for_write();
    }
    if (Curves **curves = std::get_if<Curves *>(&data)) {
      return (*curves)->geometry.wrap().attributes_for_write();
    }
    return std::nullopt;
  }
};

struct ParsedBehaviors {
  Vector<SimGeometrySet> sim_geometry_sets;
  Vector<SimForce> sim_forces;
  Vector<SimAcceleration> sim_accelerations;
  Vector<XpbdContraints *> constraints;
};

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

static ParsedBehaviors parse_behaviors(const BundlePtr &behaviors_bundle)
{
  if (!behaviors_bundle) {
    return {};
  }
  ParsedBehaviors parsed_behaviors;
  Vector<StringRef> path_stack;
  foreach_behavior_recursive(
      *behaviors_bundle,
      path_stack,
      [&](const StringRef type, const Bundle &behavior_bundle, const Span<StringRef> path_stack) {
        const std::string path = combine_bundle_path(path_stack);
        if (type == "Geometry") {
          std::optional<Bundle::Item> item = behavior_bundle.lookup(
              SocketInterfaceKey{"Geometry"});
          if (!item) {
            return;
          }
          if (item->type->type != SOCK_GEOMETRY) {
            return;
          }
          SimGeometrySet geometry;
          geometry.path = path;
          geometry.geometry = *static_cast<const GeometrySet *>(item->value);
          parsed_behaviors.sim_geometry_sets.append(geometry);
          return;
        }
        if (type == "Force") {
          std::optional<Bundle::Item> item = behavior_bundle.lookup(
              SocketInterfaceKey{"Force Field"});
          if (!item) {
            return;
          }
          if (item->type->type != SOCK_VECTOR) {
            return;
          }
          SimForce force;
          force.force_field =
              static_cast<const bke::SocketValueVariant *>(item->value)->get<Field<float3>>();
          parsed_behaviors.sim_forces.append(force);
          return;
        }
        if (type == "Acceleration") {
          std::optional<Bundle::Item> item = behavior_bundle.lookup(
              SocketInterfaceKey{"Acceleration Field"});
          if (!item) {
            return;
          }
          if (item->type->type != SOCK_VECTOR) {
            return;
          }
          SimAcceleration acceleration;
          acceleration.acceleration_field =
              static_cast<const bke::SocketValueVariant *>(item->value)->get<Field<float3>>();
          parsed_behaviors.sim_accelerations.append(acceleration);
          return;
        }
      });

  return parsed_behaviors;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr old_data_bundle = params.extract_input<BundlePtr>("Data");
  BundlePtr behaviors_bundle = params.extract_input<BundlePtr>("Behavior");
  const float delta_time = params.extract_input<float>("Delta Time");
  const int substeps = params.extract_input<int>("Substeps");

  ParsedBehaviors parsed_behaviors = parse_behaviors(behaviors_bundle);
  if (old_data_bundle) {
    for (SimGeometrySet &sim_geometry : parsed_behaviors.sim_geometry_sets) {
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

  Vector<SimGeometry> sim_geometries;

  for (SimGeometrySet &sim_geometry_set : parsed_behaviors.sim_geometry_sets) {
    if (Mesh *mesh = sim_geometry_set.geometry.get_mesh_for_write()) {
      sim_geometries.append(SimGeometry{sim_geometry_set.path, mesh});
    }
    if (PointCloud *pointcloud = sim_geometry_set.geometry.get_pointcloud_for_write()) {
      sim_geometries.append(SimGeometry{sim_geometry_set.path, pointcloud});
    }
    if (Curves *curves = sim_geometry_set.geometry.get_curves_for_write()) {
      sim_geometries.append(SimGeometry{sim_geometry_set.path, curves});
    }
  }

  const int sim_steps = 1 + std::max(0, substeps);
  const float sub_delta_time = delta_time / sim_steps;
  for ([[maybe_unused]] int substep : IndexRange(sim_steps)) {
    for (SimGeometry &sim_geometry : sim_geometries) {
      std::optional<bke::MutableAttributeAccessor> attributes =
          sim_geometry.attributes_for_write();
      if (!attributes) {
        continue;
      }
      std::optional<bke::GeometryFieldContext> field_context;
      if (Mesh **mesh = std::get_if<Mesh *>(&sim_geometry.data)) {
        field_context.emplace(**mesh, bke::AttrDomain::Point);
      }
      if (PointCloud **pointcloud = std::get_if<PointCloud *>(&sim_geometry.data)) {
        field_context.emplace(**pointcloud, bke::AttrDomain::Point);
      }
      if (Curves **curves = std::get_if<Curves *>(&sim_geometry.data)) {
        field_context.emplace(**curves, bke::AttrDomain::Point);
      }
      if (!field_context) {
        continue;
      }
      const int positions_num = attributes->domain_size(bke::AttrDomain::Point);
      Array<float3> force(positions_num, float3());
      Array<float3> acceleration(positions_num, float3());
      fn::FieldEvaluator field_evaluator{*field_context, positions_num};
      for (const SimForce &sim_force : parsed_behaviors.sim_forces) {
        field_evaluator.add(sim_force.force_field);
      }
      for (const SimAcceleration &sim_acceleration : parsed_behaviors.sim_accelerations) {
        field_evaluator.add(sim_acceleration.acceleration_field);
      }
      field_evaluator.evaluate();
      for (const int force_i : parsed_behaviors.sim_forces.index_range()) {
        VArraySpan<float3> force_varray = field_evaluator.get_evaluated<float3>(force_i);
        for (const int i : force_varray.index_range()) {
          force[i] += force_varray[i];
        }
      }
      for (const int acceleration_i : parsed_behaviors.sim_accelerations.index_range()) {
        VArraySpan<float3> acceleration_varray = field_evaluator.get_evaluated<float3>(
            acceleration_i + parsed_behaviors.sim_forces.size());
        for (const int i : acceleration_varray.index_range()) {
          acceleration[i] += acceleration_varray[i];
        }
      }
      const VArray<float> masses = *attributes->lookup_or_default<float>(
          "mass", bke::AttrDomain::Point, 1.0f);

      bke::SpanAttributeWriter<float3> velocities =
          attributes->lookup_or_add_for_write_span<float3>("velocity", bke::AttrDomain::Point);
      bke::SpanAttributeWriter<float3> positions =
          attributes->lookup_or_add_for_write_span<float3>("position", bke::AttrDomain::Point);
      bke::SpanAttributeWriter<float3> old_positions =
          attributes->lookup_or_add_for_write_span<float3>("old_position", bke::AttrDomain::Point);
      for (const int i : velocities.span.index_range()) {
        velocities.span[i] += force[i] * sub_delta_time / masses[i];
        velocities.span[i] += acceleration[i] * sub_delta_time;
        old_positions.span[i] = positions.span[i];
        positions.span[i] += velocities.span[i] * sub_delta_time;
      }
      velocities.finish();
      positions.finish();
      old_positions.finish();
    }
  }

  BundlePtr new_data_bundle = Bundle::create();
  for (SimGeometrySet &sim_geometry : parsed_behaviors.sim_geometry_sets) {
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
