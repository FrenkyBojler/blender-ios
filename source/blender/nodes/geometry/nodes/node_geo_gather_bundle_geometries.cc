/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_instances.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_gather_bundle_geometries_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bundle>("Bundle");
  b.add_output<decl::Geometry>("Geometry");
}

static void gather_geometry_sets_recursive(const Bundle &bundle,
                                           Vector<GeometrySet> &r_geometry_sets)
{
  for (const auto &item : bundle.items()) {
    if (std::optional<GeometrySet> geometry = item.value.as<GeometrySet>()) {
      r_geometry_sets.append(*geometry);
    }
    else if (const BundlePtr child_bundle = item.value.as<BundlePtr>().value_or(nullptr)) {
      gather_geometry_sets_recursive(*child_bundle, r_geometry_sets);
    }
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const BundlePtr bundle = params.extract_input<BundlePtr>("Bundle");
  if (!bundle) {
    params.set_default_remaining_outputs();
    return;
  }

  Vector<GeometrySet> geometry_sets;
  gather_geometry_sets_recursive(*bundle, geometry_sets);

  bke::Instances *instances = new bke::Instances();
  for (GeometrySet &geometry_set : geometry_sets) {
    const int handle = instances->add_reference(std::move(geometry_set));
    instances->add_instance(handle, float4x4::identity());
  }

  GeometrySet output = GeometrySet::from_instances(instances);
  params.set_output("Geometry", std::move(output));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGatherBundleGeometries");
  ntype.ui_name = "Gather Bundle Geometries";
  ntype.ui_description = "Output an instance for each geometry in the bundle";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_gather_bundle_geometries_cc
