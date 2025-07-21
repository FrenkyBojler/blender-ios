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

constexpr StringRefNull prev_position_name = ".prev_position";

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
          if (std::optional<Bundle::Item> mass_item = behavior_bundle.lookup(
                  SocketInterfaceKey{"Mass Attribute"}))
          {
            if (mass_item->type->type != SOCK_STRING) {
              return;
            }
            geometry.mass_attribute =
                static_cast<const bke::SocketValueVariant *>(mass_item->value)->get<std::string>();
          }
          if (std::optional<Bundle::Item> velocity_item = behavior_bundle.lookup(
                  SocketInterfaceKey{"Velocity Attribute"}))
          {
            if (velocity_item->type->type != SOCK_STRING) {
              return;
            }
            geometry.velocity_attribute = static_cast<const bke::SocketValueVariant *>(
                                              velocity_item->value)
                                              ->get<std::string>();
          }
          behaviors.sim_geometry_sets.append(geometry);
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
          geometry::xpbd::ForceField force;
          force.force_field =
              static_cast<const bke::SocketValueVariant *>(item->value)->get<Field<float3>>();
          behaviors.force_fields.append(force);
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
          geometry::xpbd::AccelerationField acceleration;
          acceleration.acceleration_field =
              static_cast<const bke::SocketValueVariant *>(item->value)->get<Field<float3>>();
          behaviors.acceleration_fields.append(acceleration);
          return;
        }
        if (type == "Edge Length Constraint") {
          std::optional<Bundle::Item> item = behavior_bundle.lookup(
              SocketInterfaceKey{"Rest Length Attribute"});
          if (!item) {
            return;
          }
          if (item->type->type != SOCK_STRING) {
            return;
          }
          std::string rest_length_attribute =
              static_cast<const bke::SocketValueVariant *>(item->value)->get<std::string>();
          behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__edge_lengths(
              scope, std::move(rest_length_attribute)));
          return;
        }
        if (type == "Fixed Position Constraint") {
          std::optional<Bundle::Item> selection_item = behavior_bundle.lookup(
              SocketInterfaceKey{"Selection"});
          std::optional<Bundle::Item> positions_item = behavior_bundle.lookup(
              SocketInterfaceKey{"Position"});
          if (!selection_item || !positions_item) {
            return;
          }
          if (selection_item->type->type != SOCK_BOOLEAN ||
              positions_item->type->type != SOCK_VECTOR) {
            return;
          }
          behaviors.constraint_sets.append(&geometry::xpbd::create_constraint__fixed_positions(
              scope,
              static_cast<const bke::SocketValueVariant *>(selection_item->value)
                  ->get<Field<bool>>(),
              static_cast<const bke::SocketValueVariant *>(positions_item->value)
                  ->get<Field<float3>>()));
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

  Vector<geometry::xpbd::SimGeometry> sim_geometries;

  for (geometry::xpbd::SimGeometrySet &sim_geometry_set : behaviors.sim_geometry_sets) {
    if (Mesh *mesh = sim_geometry_set.geometry.get_mesh_for_write()) {
      sim_geometries.append(geometry::xpbd::SimGeometry{sim_geometry_set, mesh});
    }
    if (PointCloud *pointcloud = sim_geometry_set.geometry.get_pointcloud_for_write()) {
      sim_geometries.append(geometry::xpbd::SimGeometry{sim_geometry_set, pointcloud});
    }
    if (Curves *curves = sim_geometry_set.geometry.get_curves_for_write()) {
      sim_geometries.append(geometry::xpbd::SimGeometry{sim_geometry_set, curves});
    }
  }

  const int sim_steps = 1 + substeps;
  const float sub_delta_time = delta_time / sim_steps;
  for ([[maybe_unused]] int substep : IndexRange(sim_steps)) {
    /* Remember previous positions. */
    for (geometry::xpbd::SimGeometry &sim_geometry : sim_geometries) {
      std::optional<bke::MutableAttributeAccessor> attributes =
          sim_geometry.attributes_for_write();
      if (!attributes) {
        continue;
      }
      const bke::AttributeReader<float3> positions = attributes->lookup<float3>("position");
      attributes->remove(prev_position_name);
      attributes->add<float3>(prev_position_name,
                              AttrDomain::Point,
                              bke::AttributeInitShared{positions.varray.get_internal_span().data(),
                                                       *positions.sharing_info});
    }

    /* Init constraints. */
    for (geometry::xpbd::ConstraintSet *constraint : behaviors.constraint_sets) {
      constraint->ensure_init(sim_geometries);
    }

    /* Handle forces and accelerations. If the time step is zero, these can't have any effect. */
    if (sub_delta_time > 0) {
      for (geometry::xpbd::SimGeometry &sim_geometry : sim_geometries) {
        std::optional<bke::MutableAttributeAccessor> attributes =
            sim_geometry.attributes_for_write();
        if (!attributes) {
          continue;
        }
        std::optional<bke::GeometryFieldContext> field_context;
        sim_geometry.set_point_field_context(field_context);
        if (!field_context) {
          continue;
        }
        const int positions_num = attributes->domain_size(bke::AttrDomain::Point);
        Array<float3> force(positions_num, float3());
        Array<float3> acceleration(positions_num, float3());
        fn::FieldEvaluator field_evaluator{*field_context, positions_num};
        for (const geometry::xpbd::ForceField &sim_force : behaviors.force_fields) {
          field_evaluator.add(sim_force.force_field);
        }
        for (const geometry::xpbd::AccelerationField &sim_acceleration :
             behaviors.acceleration_fields)
        {
          field_evaluator.add(sim_acceleration.acceleration_field);
        }
        field_evaluator.evaluate();
        for (const int force_i : behaviors.force_fields.index_range()) {
          VArraySpan<float3> force_varray = field_evaluator.get_evaluated<float3>(force_i);
          for (const int i : force_varray.index_range()) {
            force[i] += force_varray[i];
          }
        }
        for (const int acceleration_i : behaviors.acceleration_fields.index_range()) {
          VArraySpan<float3> acceleration_varray = field_evaluator.get_evaluated<float3>(
              acceleration_i + behaviors.force_fields.size());
          for (const int i : acceleration_varray.index_range()) {
            acceleration[i] += acceleration_varray[i];
          }
        }
        const VArray<float> masses = *attributes->lookup_or_default<float>(
            sim_geometry.mass_attribute, bke::AttrDomain::Point, 1.0f);

        bke::SpanAttributeWriter<float3> velocities =
            attributes->lookup_or_add_for_write_span<float3>(sim_geometry.velocity_attribute,
                                                             bke::AttrDomain::Point);
        bke::SpanAttributeWriter<float3> positions =
            attributes->lookup_or_add_for_write_span<float3>("position", bke::AttrDomain::Point);
        for (const int i : velocities.span.index_range()) {
          velocities.span[i] += force[i] * sub_delta_time / masses[i];
          velocities.span[i] += acceleration[i] * sub_delta_time;
          positions.span[i] += velocities.span[i] * sub_delta_time;
        }
        velocities.finish();
        positions.finish();
      }
    }

    /* Constraint solve step. */
    geometry::xpbd::ConstraintCorrections corrections(sim_geometries);
    threading::parallel_for(
        behaviors.constraint_sets.index_range(), 1, [&](const IndexRange range) {
          for (const int constraint_i : range) {
            geometry::xpbd::ConstraintSet *constraints = behaviors.constraint_sets[constraint_i];
            constraints->solve(sim_geometries, corrections);
          }
        });
    corrections.apply();

    /* Apply hard constraints. */
    for (geometry::xpbd::ConstraintSet *constraint : behaviors.constraint_sets) {
      constraint->post_solve_apply(sim_geometries);
    }

    /* Write back velocities. Velocities can't be computed if the time step is zero. */
    if (sub_delta_time > 0) {
      for (geometry::xpbd::SimGeometry &sim_geometry : sim_geometries) {
        std::optional<bke::MutableAttributeAccessor> attributes =
            sim_geometry.attributes_for_write();
        if (!attributes) {
          continue;
        }
        const VArraySpan<float3> prev_positions = *attributes->lookup<float3>(prev_position_name);
        const VArraySpan<float3> positions = *attributes->lookup<float3>("position");
        bke::SpanAttributeWriter<float3> velocities = attributes->lookup_for_write_span<float3>(
            sim_geometry.velocity_attribute);
        threading::parallel_for(positions.index_range(), 512, [&](const IndexRange range) {
          for (const int i : range) {
            velocities.span[i] = (positions[i] - prev_positions[i]) / sub_delta_time;
          }
        });
        velocities.finish();
      }
    }

    /* Remove temporary attributes.*/
    for (geometry::xpbd::SimGeometry &sim_geometry : sim_geometries) {
      std::optional<bke::MutableAttributeAccessor> attributes =
          sim_geometry.attributes_for_write();
      if (!attributes) {
        continue;
      }
      attributes->remove(prev_position_name);
    }
  }

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
