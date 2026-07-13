// A node that gets the custom property of an ID.

/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_socket_search_link.hh"

#include "BKE_idprop.hh"

namespace blender::nodes::node_geo_get_custom_property {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::DataBlockID>("ID"_ustr).description("ID to get the custom property of");
  b.add_input<decl::String>("Name"_ustr).description("Name of the custom property");
  b.add_output<decl::Bool>("Exists"_ustr).default_value(false);
  // TODO: add a data type socket & a dynamic socket for the return type.
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const ID *id = params.extract_input<ID *>("ID"_ustr);

  if (id == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  const std::string name = params.extract_input<std::string>("Name"_ustr);

  for (IDProperty* idp = id->properties; idp != nullptr; idp = idp->next) {
    if (strcmp(idp->name, name.c_str()) == 0) {
        params.set_output("Exists"_ustr, true);
        break;
    }
  }

  // TODO: find the custom property
//   params.set_output("Geometry"_ustr, std::move(geometry_set));
//   params.set_output("Bundle"_ustr, std::move(bundle));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGetCustomProperty"_ustr);
  ntype.ui_name = "Get Custom Property";
  ntype.ui_description = "Get the custom property of an ID";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.default_width = bke::NodeWidth::_160;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_get_custom_property
