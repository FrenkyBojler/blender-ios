/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_utils.hh"

#include "NOD_geometry_nodes_bundle.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_join_bundles_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bundle>("Bundles").multi_input();
  b.add_output<decl::Bundle>("Bundle").align_with_previous();
  b.add_input<decl::Bool>("Make Names Unique").default_value(false);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  auto bundles = params.extract_input<GeoNodesMultiInput<BundlePtr>>("Bundles");
  if (bundles.values.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }
  const bool make_names_unique = params.extract_input<bool>("Make Names Unique");

  BundlePtr output_bundle_ptr = Bundle::create();
  Bundle &output_bundle = const_cast<Bundle &>(*output_bundle_ptr);
  for (const BundlePtr &bundle_ptr : bundles.values) {
    if (!bundle_ptr) {
      continue;
    }
    for (const Bundle::StoredItem &item : bundle_ptr->items()) {
      if (output_bundle.add(item.key, item.value)) {
        /* Name was unique and has been added. */
        continue;
      }
      if (!make_names_unique) {
        /* Ignore the item because the name exists already. */
        continue;
      }
      const std::string name = BLI_uniquename_cb(
          [&](const StringRef name_to_test) { return output_bundle.contains(name_to_test); },
          '_',
          item.key);
      output_bundle.add_new(name, item.value);
    }
  }

  params.set_output("Bundle", std::move(output_bundle_ptr));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "NodeJoinBundles");
  ntype.ui_name = "Join Bundles";
  ntype.ui_description = "Combine multiple bundles into one";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_join_bundles_cc
