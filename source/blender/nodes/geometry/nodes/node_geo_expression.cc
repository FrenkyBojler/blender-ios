/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>
#include <sstream>

#include <fmt/format.h>

#include "BLI_dot_export.hh"

#include "NOD_expression_parse.hh"

#include "node_geometry_util.hh"
#include "shader/node_shader_util.hh"

namespace blender::nodes::node_geo_expression_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_output<decl::Int>("Value");
  b.add_input<decl::String>("Expression").optional_label();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const std::string expression = params.extract_input<std::string>("Expression");

  std::stringstream errors;
  ResourceScope scope;
  const expression::ast::Expr *value = expression::parse(scope, expression, errors);
  if (!value) {
    params.error_message_add(NodeWarningType::Error, errors.str());
    params.set_default_remaining_outputs();
    return;
  }

  dot_export::DirectedGraph graph;
  graph.attributes.set("ordering", "out");
  value->to_dot(graph);
  std::cout << "\n\n" << graph.to_dot_string() << "\n\n";
  params.set_default_remaining_outputs();
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  sh_geo_node_type_base(&ntype, "NodeExpression");
  ntype.ui_name = "Expression";
  ntype.ui_description = "Evaluate an expression on inputs";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_expression_cc
