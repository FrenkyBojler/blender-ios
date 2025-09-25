/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "BKE_mesh.hh"
#include "BLI_math_vector_types.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_join_bundle {
static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bundle>("Bundle").multi_input().description(
      "Bundles to join together on the top level for each bundle.");
  b.add_output<decl::Bundle>("Bundle");
}
static void node_geo_exec(GeoNodeExecParams params)
{
  GeoNodesMultiInput<BundlePtr> bundles = params.extract_input<GeoNodesMultiInput<BundlePtr>>(
      "Bundle");

  if (bundles.values.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  BundlePtr output_bundle = Bundle::create();

  Bundle &mutable_output_bundle = const_cast<Bundle &>(*output_bundle);

  for (BundlePtr &bundle : bundles.values) {
    if (!bundle) {
      continue;
    }
    for (const Bundle::StoredItem &item : bundle->items()) {
      mutable_output_bundle.add(item.key, item.value);
    }
  }

  params.set_output("Bundle", output_bundle);
}
static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "NodeJoinBundle");
  ntype.ui_name = "Join Bundle";
  ntype.ui_description = "Join multiple bundles together";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_geo_join_bundle
