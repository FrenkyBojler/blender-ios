// A node that gets the custom property of an ID.

/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_socket_search_link.hh"

#include "BKE_idprop.hh"

namespace blender::nodes::node_geo_get_custom_property {

static const EnumPropertyItem type_items[] = {
    {IDP_FLOAT, "FLOAT", 0, N_("Float"), N_("Floating-point value")},
    {IDP_BOOLEAN, "BOOLEAN", 0, N_("Boolean"), N_("Boolean value")},
    {IDP_INT, "INTEGER", 0, N_("Integer"), N_("Integer value")},
    {IDP_STRING, "STRING", 0, N_("String"), N_("String value")},
    {0, nullptr, 0, nullptr, nullptr},
};

#define ADD_TYPED_OUTPUT(decl_type, idp_type)\
  b.add_output<decl_type>("Value"_ustr)\
    .make_available([](bNode& node) {\
        bNodeSocket &type_socket = *bke::node_find_socket(node, SOCK_IN, "Type"_ustr);\
        type_socket.default_value_typed<bNodeSocketValueMenu>()->value = idp_type;\
    })\
    .usage_by_single_menu(idp_type)\
    .optional_label();

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Menu>("Type"_ustr)
    .default_value(IDP_FLOAT)
    .static_items(type_items)
    .optional_label();

  ADD_TYPED_OUTPUT(decl::Bool, IDP_BOOLEAN)
  ADD_TYPED_OUTPUT(decl::Float, IDP_FLOAT)
  ADD_TYPED_OUTPUT(decl::Int, IDP_INT)
  ADD_TYPED_OUTPUT(decl::String, IDP_STRING)

  b.add_output<decl::Bool>("Exists"_ustr).default_value(false);

  b.add_input<decl::DataBlockID>("ID"_ustr).description("ID to get the custom property of");
  b.add_input<decl::String>("Name"_ustr).description("Name of the custom property");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  ID *id = params.extract_input<ID *>("ID"_ustr);

  if (id == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  const std::string name = params.extract_input<std::string>("Name"_ustr);
  const auto type = params.extract_input<eIDPropertyType>("Type"_ustr);

  // TODO: IDP have groups.
  // The user properties are in a top-level group called `user_properties`.
  // We need to be able to recurse into the groups, however we can't rely on the names to make an
  // addressing scheme using `.` as a separator, for example.
  // We could add yet another socket type for this but that's probably overkill.
  // Groups can be recursive.
  IDProperty* group = IDP_GetProperties(id);
  if (group == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  std::cout << "we get the group baby" << std::endl;
  std::cout << "looking for " << name << std::endl;

  IDProperty* prop = IDP_GetPropertyTypeFromGroup(group, name, type);
  if (prop == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  std::cout << "we got the prop" << std::endl;

  params.set_output("Exists"_ustr, true);

  switch (type) {
    case IDP_STRING:
        params.set_output("Value"_ustr, std::string(IDP_string_get(prop))); // maybe an issue with encoding?
        return;
    case IDP_INT:
        params.set_output("Value"_ustr, IDP_int_get(prop));
        return;
    case IDP_FLOAT:
        params.set_output("Value"_ustr, IDP_float_get(prop));
        return;
    case IDP_BOOLEAN:
        params.set_output("Value"_ustr, IDP_bool_get(prop));
        return;
    default:
        BLI_assert_unreachable();
  }

  params.set_default_remaining_outputs();
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGetCustomProperty"_ustr);
  ntype.ui_name = "Get Custom Property";
  ntype.ui_description = "Get the custom property of an ID";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.default_width = bke::NodeWidth::_160;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_get_custom_property
