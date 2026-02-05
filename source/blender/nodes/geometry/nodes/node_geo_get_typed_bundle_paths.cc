/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_bundle.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_get_typed_bundle_paths_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bundle>("Bundle");
  b.add_input<decl::String>("Type");
  b.add_output<decl::String>("Paths").structure_type(StructureType::List);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr bundle = params.extract_input<BundlePtr>("Bundle");
  const std::string type_filter = params.extract_input<std::string>("Type");

  Vector<std::string> paths;
  if (bundle) {
    paths = gather_bundle_paths_by_type(*bundle, type_filter);
  }

  params.set_output("Paths", List::from_container(std::move(paths)));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "NodeGetTypedBundlePaths");
  ntype.ui_name = "Get Typed Bundle Paths";
  ntype.ui_description = "Get paths to nested typed bundles";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_get_typed_bundle_paths_cc
