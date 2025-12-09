/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_data_block_to_string_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Object>("Data Block").optional_label();
  b.add_output<decl::String>("String");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Object *data_block = params.extract_input<Object *>("Data Block");

  std::string output = "";
  if (data_block != nullptr) {
    output = std::string(data_block->id.name);
  }
  params.set_output("String", std::move(output));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeDataBlockToString");
  ntype.ui_name = "Data Block To String";
  ntype.ui_description = "Convert data block to string";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_data_block_to_string_cc
