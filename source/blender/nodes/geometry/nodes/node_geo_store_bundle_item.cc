/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_geo_bundle.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"
#include "NOD_sync_sockets.hh"

#include "BKE_idprop.hh"

#include "BLO_read_write.hh"

#include "NOD_geometry_nodes_bundle.hh"

#include "UI_interface_layout.hh"

namespace blender::nodes::node_geo_store_bundle_item_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Bundle>("Bundle");
  b.add_output<decl::Bundle>("Bundle").align_with_previous();
  b.add_input<decl::String>("Name").optional_label();
  b.add_input<decl::Vector>("Item");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &bnode = params.node();

  BundlePtr bundle_ptr = params.extract_input<nodes::BundlePtr>("Bundle");
  if (!bundle_ptr) {
    bundle_ptr = Bundle::create();
  }

  Bundle &bundle = const_cast<Bundle &>(*bundle_ptr);

  const std::string name = params.extract_input<std::string>("Name");
  if (name.empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  bke::SocketValueVariant value = params.extract_input<bke::SocketValueVariant>("Item");
  const bNodeSocket *item_sock = bnode.input_by_identifier("Item");
  if (!item_sock) {
    params.set_output("Bundle", std::move(bundle_ptr));
    return;
  }
  
  const bke::bNodeSocketType *stype = bke::node_socket_type_find_static(SOCK_VECTOR, 0);
  if (!stype || !stype->geometry_nodes_default_value) {
    params.set_output("Bundle", std::move(bundle_ptr));
    return;
  }

  bundle.add_override(name, BundleItemSocketValue{stype, std::move(value)});

  params.set_output("Bundle", std::move(bundle_ptr));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "NodeStoreBundleItem");
  ntype.ui_name = "Store Bundle Item";
  ntype.ui_description = "Store a bundle item by name.";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_store_bundle_item_cc
