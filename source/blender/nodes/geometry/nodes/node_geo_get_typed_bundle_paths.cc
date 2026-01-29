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

static void gather_bundle_paths_recursive(const BundlePtr &bundle_ptr,
                                          const StringRef self_path,
                                          const StringRef type_filter,
                                          Vector<std::string> &r_paths)
{
  if (!bundle_ptr) {
    return;
  }
  const std::optional<StringRef> bundle_type = bundle_ptr->type();
  if (bundle_type.has_value()) {
    if (!type_filter.is_empty()) {
      if (*bundle_type != type_filter) {
        return;
      }
    }
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
    gather_bundle_paths_recursive(*child_bundle_ptr, child_path, type_filter, r_paths);
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  BundlePtr bundle = params.extract_input<BundlePtr>("Bundle");
  const std::string type_filter = params.extract_input<std::string>("Type");
  Vector<std::string> paths;
  gather_bundle_paths_recursive(bundle, "", type_filter, paths);

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
