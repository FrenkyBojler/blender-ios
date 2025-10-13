/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "ED_screen.hh"

#include "NOD_geo_bundle.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_sync_sockets.hh"

#include "BKE_idprop.hh"

#include "BLO_read_write.hh"

#include "UI_interface_layout.hh"

#include <fmt/format.h>

namespace blender::nodes::node_geo_get_named_bundle_cc {

typedef enum NodeMenu {
  GEO_NODE_1 = 0,
  GEO_NODE_2 = 1,
} NodeMenu;

static EnumPropertyItem type_items[] = {
    {GEO_NODE_1, "VECTOR", 0, N_("Vector"), N_("Vector 1")},
    {GEO_NODE_2, "MATRIX", 0, N_("Matrix"), N_("Matrix 2")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Bundle>("Bundle");
  b.add_output<decl::Bundle>("Bundle").align_with_previous();
  b.add_output<decl::Vector>("Item");
  b.add_output<decl::Bool>("Exists");
  b.add_input<decl::Menu>("Type").static_items(type_items).optional_label();
  b.add_input<decl::String>("Name").optional_label();
  b.add_input<decl::Bool>("Remove");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  nodes::BundlePtr bundle = params.extract_input<nodes::BundlePtr>("Bundle");
  if (!bundle) {
    params.set_output("Exists", false);
    params.set_default_remaining_outputs();
    return;
  }

  const StringRef name = params.extract_input<std::string>("Name");
  const bool remove = params.extract_input<bool>("Remove");

  if (name.is_empty()) {
    params.set_output("Exists", false);
    params.set_default_remaining_outputs();
    return;
  }

  const BundleItemValue *value = bundle->lookup(name);
  if (!value) {
    params.set_output("Exists", false);
    params.set_default_remaining_outputs();
    return;
  }
  const auto *socket_value = std::get_if<BundleItemSocketValue>(&value->value);
  if (!socket_value) {
    params.error_message_add(
        NodeWarningType::Error,
        fmt::format("{}: \"{}\"", TIP_("Cannot get internal value from bundle"), name));
    params.set_output("Exists", false);
    params.set_default_remaining_outputs();
    return;
  }


  if (remove) {
  }

  params.set_output("Bundle", std::move(bundle));
  params.set_output("Item", socket_value->value);
  params.set_output("Exists", true);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "NodeGetNamedBundle");
  ntype.ui_name = "Get Named Bundle";
  ntype.ui_description = "Retrieve a bundle item by name.";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_get_named_bundle_cc
