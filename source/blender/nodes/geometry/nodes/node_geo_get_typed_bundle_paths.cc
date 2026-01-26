/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_bundle.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_get_typed_bundle_paths_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bundle>("Bundle");
  b.add_output<decl::String>("Paths").structure_type(StructureType::List);
}

static void gather_bundle_paths_recursive(const BundlePtr &bundle_ptr,
                                          const StringRef self_path,
                                          Vector<std::string> &r_paths)
{
  if (!bundle_ptr) {
    return;
  }
  if (bundle_ptr->type().has_value()) {
    r_paths.append(self_path);
    return;
  }
  for (const auto &item : bundle_ptr->items()) {
    const BundlePtr *child_bundle_ptr = item.value.as_pointer<BundlePtr>();
    if (!child_bundle_ptr) {
      continue;
    }
    const std::string child_path = self_path.is_empty() ?
                                       item.key :
                                       fmt::format("{}/{}", self_path, item.key);
    gather_bundle_paths_recursive(*child_bundle_ptr, child_path, r_paths);
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr bundle = params.extract_input<BundlePtr>("Bundle");
  auto *paths = new ImplicitSharedValue<Vector<std::string>>();
  gather_bundle_paths_recursive(bundle, "", paths->data);

  List::ArrayData paths_array_data = {paths->data.data(), ImplicitSharingPtr<>(paths)};
  params.set_output(
      "Paths",
      List::create(CPPType::get<std::string>(), std::move(paths_array_data), paths->data.size()));
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
