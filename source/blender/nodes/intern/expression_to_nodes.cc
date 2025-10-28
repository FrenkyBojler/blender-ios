/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <sstream>

#include <fmt/format.h>

#include "BLI_listbase.h"
#include "BLI_resource_scope.hh"
#include "BLT_translation.hh"

#include "NOD_expression_parse.hh"
#include "NOD_expression_to_nodes.hh"

#include "BKE_node.hh"

namespace blender::nodes::expression {

struct NodeAndSocket {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;
};

class AstToNodeGroupBuilder {
 private:
  const bNode &expr_bnode_;
  const ast::Expr &root_expr_;
  bNodeTree &r_tree_;
  Map<StringRef, NodeAndSocket> inputs_;

  std::string &r_error_;

 public:
  AstToNodeGroupBuilder(const bNode &expr_bnode,
                        const ast::Expr &root_expr,
                        bNodeTree &r_tree,
                        std::string &r_error)
      : expr_bnode_(expr_bnode), root_expr_(root_expr), r_tree_(r_tree), r_error_(r_error)
  {
  }

  void build()
  {
    Map<StringRef, bNodeSocket *> expr_inputs;
    for (const int i : expr_bnode_.input_sockets().index_range()) {
      const bNodeSocket &socket = expr_bnode_.input_socket(i);
      r_tree_.tree_interface.add_socket(
          socket.name, "", socket.idname, NODE_INTERFACE_SOCKET_INPUT, nullptr);
    }
    for (const int i : expr_bnode_.output_sockets().index_range()) {
      const bNodeSocket &socket = expr_bnode_.output_socket(i);
      r_tree_.tree_interface.add_socket(
          socket.name, "", socket.idname, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
    }

    bNode &group_input_node = this->add_node("NodeGroupInput");
    bNode &group_output_node = this->add_node("NodeGroupOutput");

    LISTBASE_FOREACH (bNodeSocket *, socket, &group_input_node.outputs) {
      /* TODO: Generalize this. */
      if (socket == group_input_node.outputs.first) {
        continue;
      }
      inputs_.add(socket->name, {&group_input_node, socket});
    }

    NodeAndSocket expr_result = this->build_expr(root_expr_);
    if (!expr_result.socket) {
      return;
    }

    bke::node_add_link(r_tree_,
                       *expr_result.node,
                       *expr_result.socket,
                       group_output_node,
                       *static_cast<bNodeSocket *>(group_output_node.inputs.first));
  }

 private:
  NodeAndSocket build_expr(const ast::Expr &expr)
  {
    return std::visit([&](const auto &ast_node) { return this->build_expr(ast_node); }, expr.expr);
  }

  NodeAndSocket build_expr(const ast::Number &ast_node)
  {
    const int value = std::stoi(ast_node.value);
    bNode &node = this->add_node("ShaderNodeValue");
    bNodeSocket *socket = static_cast<bNodeSocket *>(node.outputs.first);
    socket->default_value_typed<bNodeSocketValueFloat>()->value = value;
    return {&node, socket};
  }

  NodeAndSocket build_expr(const ast::Identifier &ast_node)
  {
    NodeAndSocket input = inputs_.lookup_default(ast_node.identifier, {});
    if (!input.socket) {
      r_error_ = fmt::format("{}: {}", TIP_("Unknown variable"), ast_node.identifier);
      return {};
    }
    return input;
  }

  NodeAndSocket build_expr(const ast::BinaryOp & /*ast_node*/)
  {
    r_error_ = "Binary operators are not supported yet";
    return {};
  }

  NodeAndSocket build_expr(const ast::UnaryOp & /*ast_node*/)
  {
    r_error_ = "Unary operators are not supported yet";
    return {};
  }

  NodeAndSocket build_expr(const ast::MemberAccess & /*ast_node*/)
  {
    r_error_ = "Member access is not supported yet";
    return {};
  }

  NodeAndSocket build_expr(const ast::Call & /*ast_node*/)
  {
    r_error_ = "Function calls are not supported yet";
    return {};
  }

  bNode &add_node(const StringRefNull idname)
  {
    return *bke::node_add_node(nullptr, r_tree_, idname);
  }
};

void expression_node_to_group(const bNode &node,
                              StringRef expression,
                              bNodeTree &r_tree,
                              std::string &r_error)
{
  std::stringstream errors;

  ResourceScope parse_scope;
  ast::Expr *expr_ast = expression::parse(parse_scope, expression, errors);
  if (!expr_ast) {
    r_error = errors.str();
    if (r_error.empty()) {
      r_error = TIP_("Parse error");
    }
    return;
  }

  AstToNodeGroupBuilder builder(node, *expr_ast, r_tree, r_error);
  builder.build();
}

}  // namespace blender::nodes::expression
