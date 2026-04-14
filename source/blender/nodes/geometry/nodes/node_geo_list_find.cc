/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_list_find_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>("Boolean"_ustr)
      .structure_type(StructureType::List)
      .hide_value()
      .description("Boolean list to search");

  b.add_output<decl::Int>("Indices"_ustr)
      .structure_type(StructureType::List)
      .description("Indices where value is true");
  b.add_output<decl::Int>("Count"_ustr).description("Number of true values found");
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  if (!U.experimental.use_geometry_nodes_lists) {
    return;
  }
  if (params.in_out() == SOCK_IN) {
    if (params.other_socket().type == SOCK_BOOLEAN) {
      params.add_item(IFACE_("Boolean"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeListFind"_ustr);
        params.update_and_connect_available_socket(node, "Boolean"_ustr);
      });
    }
  }
  else {
    if (params.other_socket().type == SOCK_INT) {
      params.add_item(IFACE_("Indices"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeListFind"_ustr);
        params.update_and_connect_available_socket(node, "Indices"_ustr);
      });
      params.add_item(IFACE_("Count"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeListFind"_ustr);
        params.update_and_connect_available_socket(node, "Count"_ustr);
      });
    }
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  ListPtr bool_list = params.extract_input<ListPtr>("Boolean"_ustr);

  if (!bool_list) {
    params.set_default_remaining_outputs();
    return;
  }

  const int list_size = bool_list->size();

  Vector<int> matching_indices;
  const VArray<bool> bool_varray = bool_list->varray<bool>();

  for (int i = 0; i < list_size; i++) {
    if (bool_varray[i]) {
      matching_indices.append(i);
    }
  }

  const int count = matching_indices.size();

  if (params.output_is_required("Indices"_ustr)) {
    const CPPType &int_type = CPPType::get<int>();
    if (count == 0) {
      List::ArrayData indices_data = List::ArrayData::ForDefaultValue(int_type, 0);
      ListPtr indices_list = List::create(int_type, std::move(indices_data), 0);
      params.set_output("Indices"_ustr, std::move(indices_list));
    }
    else {
      List::ArrayData indices_data = List::ArrayData::ForUninitialized(int_type, count);
      MutableSpan<int> indices_span = indices_data.span_for_write(int_type, count).typed<int>();
      indices_span.copy_from(matching_indices);
      ListPtr indices_list = List::create(int_type, std::move(indices_data), count);
      params.set_output("Indices"_ustr, std::move(indices_list));
    }
  }

  if (params.output_is_required("Count"_ustr)) {
    params.set_output("Count"_ustr, count);
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeListFind"_ustr);
  ntype.ui_name = "Find in List";
  ntype.ui_description = "Find indices where a boolean list is true";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_find_cc
