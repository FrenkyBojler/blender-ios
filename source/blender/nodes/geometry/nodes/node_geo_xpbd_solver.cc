/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "DNA_mesh_types.h"
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

struct SimGeometrySet {
  std::string path;
  GeometrySet geometry;
};

struct ParsedBehaviors {
  Vector<SimGeometrySet> sim_geometries;
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
  if (item->type->type == SOCK_BUNDLE) {
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
          parsed_behaviors.sim_geometries.append(geometry);
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
    for (SimGeometrySet &sim_geometry : parsed_behaviors.sim_geometries) {
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

  for (SimGeometrySet &sim_geometry : parsed_behaviors.sim_geometries) {
    if (sim_geometry.geometry.has_mesh()) {
      bke::MeshComponent &mesh_component =
          sim_geometry.geometry.get_component_for_write<bke::MeshComponent>();
      Mesh *mesh = mesh_component.get_for_write();
      if (mesh) {
        MutableSpan<float3> positions = mesh->vert_positions_for_write();
        for (float3 &p : positions) {
          p.z += 1.0f * delta_time;
        }
        mesh->tag_positions_changed();
      }
    }
  }

  BundlePtr new_data_bundle = Bundle::create();
  for (SimGeometrySet &sim_geometry : parsed_behaviors.sim_geometries) {
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
