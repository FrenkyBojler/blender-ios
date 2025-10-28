/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <charconv>
#include <iostream>

#include <ctpg/ctpg.hpp>
#include <fmt/format.h>

#include "BLI_dot_export.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_expression_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_output<decl::Int>("Value");
  b.add_input<decl::String>("Expression").optional_label();
}

namespace ast {

class Expr;

class Number {
 public:
  std::string_view value;

  dot_export::Node &to_dot(dot_export::DirectedGraph &graph) const;
};

class Identifier {
 public:
  std::string_view identifier;

  dot_export::Node &to_dot(dot_export::DirectedGraph &graph) const;
};

class MemberAccess {
 public:
  Expr *expr = nullptr;
  std::string_view identifier;

  dot_export::Node &to_dot(dot_export::DirectedGraph &graph) const;
};

class BinaryOp {
 public:
  std::string_view op;
  Expr *a = nullptr;
  Expr *b = nullptr;

  dot_export::Node &to_dot(dot_export::DirectedGraph &graph) const;
};

class UnaryOp {
 public:
  std::string_view op;
  Expr *expr = nullptr;

  dot_export::Node &to_dot(dot_export::DirectedGraph &graph) const;
};

class Call {
 public:
  std::string_view identifier;
  Vector<Expr *> args;

  dot_export::Node &to_dot(dot_export::DirectedGraph &graph) const;
};

class Expr {
 public:
  using ExprVariant = std::variant<Number, Identifier, BinaryOp, UnaryOp, MemberAccess>;

  ExprVariant expr;

  Expr(ExprVariant expr) : expr(std::move(expr)) {}

  dot_export::Node &to_dot(dot_export::DirectedGraph &graph) const;
};

dot_export::Node &Number::to_dot(dot_export::DirectedGraph &graph) const
{
  return graph.new_node(this->value);
}

dot_export::Node &Identifier::to_dot(dot_export::DirectedGraph &graph) const
{
  return graph.new_node(this->identifier);
}

dot_export::Node &MemberAccess::to_dot(dot_export::DirectedGraph &graph) const
{
  dot_export::Node &expr_node = this->expr->to_dot(graph);
  dot_export::Node &member_access_node = graph.new_node(fmt::format(".{}", this->identifier));
  graph.new_edge(member_access_node, expr_node);
  return member_access_node;
}

dot_export::Node &BinaryOp::to_dot(dot_export::DirectedGraph &graph) const
{
  dot_export::Node &a_node = this->a->to_dot(graph);
  dot_export::Node &b_node = this->b->to_dot(graph);
  dot_export::Node &op_node = graph.new_node(this->op);
  graph.new_edge(op_node, a_node);
  graph.new_edge(op_node, b_node);
  return op_node;
}

dot_export::Node &UnaryOp::to_dot(dot_export::DirectedGraph &graph) const
{
  dot_export::Node &expr_node = this->expr->to_dot(graph);
  dot_export::Node &op_node = graph.new_node(this->op);
  graph.new_edge(op_node, expr_node);
  return op_node;
}

dot_export::Node &Call::to_dot(dot_export::DirectedGraph &graph) const
{
  dot_export::Node &identifier_node = graph.new_node(fmt::format("{}(...)", this->identifier));
  for (const Expr *arg : this->args) {
    dot_export::Node &arg_node = arg->to_dot(graph);
    graph.new_edge(identifier_node, arg_node);
  }
  return identifier_node;
}

dot_export::Node &Expr::to_dot(dot_export::DirectedGraph &graph) const
{
  return std::visit([&](const auto &expr) -> dot_export::Node & { return expr.to_dot(graph); },
                    this->expr);
}

}  // namespace ast

class ParseContext {
 public:
  ResourceScope &scope;
};

namespace grammar {
using namespace ctpg;

constexpr nterm<ast::Expr *> expr("expr");
constexpr nterm<Vector<ast::Expr *>> expr_list("args");

constexpr char number_pattern[] = "[1-9][0-9]*";
constexpr regex_term<number_pattern> number("number");

constexpr char identifier_pattern[] = "[a-zA-Z][a-zA-Z0-9]*";
constexpr regex_term<identifier_pattern> identifier("identifier");

constexpr char_term op_plus('+', 1);
constexpr char_term op_minus('-', 1);
constexpr char_term op_multiply('*', 2);
constexpr char_term op_divide('/', 2);

const int unary_minus_precedence = 3;

constexpr char_term op_member_access('.', 4);

constexpr parser p(
    expr,
    terms(
        op_plus, op_minus, op_multiply, op_divide, number, identifier, '(', ')', op_member_access),
    nterms(expr, expr_list),
    rules(
        expr(number) >>=
        [](ParseContext &ctx, const term_value<std::string_view> &v_number) {
          return &ctx.scope.construct<ast::Expr>(ast::Number{v_number.get_value()});
        },
        expr(identifier) >>=
        [](ParseContext &ctx, const term_value<std::string_view> &v_identifier) {
          return &ctx.scope.construct<ast::Expr>(ast::Identifier{v_identifier.get_value()});
        },
        expr(expr, op_member_access, identifier) >>=
        [](ParseContext &ctx,
           ast::Expr *v_expr,
           char /*skip*/,
           const term_value<std::string_view> &v_identifier) {
          return &ctx.scope.construct<ast::Expr>(ast::MemberAccess{v_expr, v_identifier});
        },
        expr(expr, op_plus, expr) >>=
        [](ParseContext &ctx, ast::Expr *v_expr_a, char /*skip*/, ast::Expr *v_expr_b) {
          return &ctx.scope.construct<ast::Expr>(ast::BinaryOp{"+", v_expr_a, v_expr_b});
        },
        expr(expr, op_minus, expr) >>=
        [](ParseContext &ctx, ast::Expr *v_expr_a, char /*skip*/, ast::Expr *v_expr_b) {
          return &ctx.scope.construct<ast::Expr>(ast::BinaryOp{"-", v_expr_a, v_expr_b});
        },
        expr(expr, op_multiply, expr) >>=
        [](ParseContext &ctx, ast::Expr *v_expr_a, char /*skip*/, ast::Expr *v_expr_b) {
          return &ctx.scope.construct<ast::Expr>(ast::BinaryOp{"*", v_expr_a, v_expr_b});
        },
        expr(expr, op_divide, expr) >>=
        [](ParseContext &ctx, ast::Expr *v_expr_a, char /*skip*/, ast::Expr *v_expr_b) {
          return &ctx.scope.construct<ast::Expr>(ast::BinaryOp{"/", v_expr_a, v_expr_b});
        },
        expr(op_minus, expr)[unary_minus_precedence] >>=
        [](ParseContext &ctx, char /*skip*/, ast::Expr *v_expr) {
          return &ctx.scope.construct<ast::Expr>(ast::UnaryOp{"-", v_expr});
        },
        expr('(', expr, ')') >>= [](ParseContext & /*ctx*/,
                                    char /*skip*/,
                                    ast::Expr *v_expr,
                                    char /*skip*/) { return v_expr; },
        expr_list() >>= [](ParseContext & /*ctx*/) { return Vector<ast::Expr *>{}; }

        ));

}  // namespace grammar

static void node_geo_exec(GeoNodeExecParams params)
{
  const std::string expression = params.extract_input<std::string>("Expression");

  ResourceScope scope;
  ParseContext parse_ctx{scope};
  auto result = grammar::p.context_parse(
      parse_ctx, ctpg::buffers::string_view_buffer(expression), std::cerr);
  if (!result.has_value()) {
    params.set_default_remaining_outputs();
    return;
  }
  const ast::Expr *value = *result;
  dot_export::DirectedGraph graph;
  graph.attributes.set("ordering", "out");
  value->to_dot(graph);
  std::cout << "\n\n" << graph.to_dot_string() << "\n\n";
  params.set_default_remaining_outputs();
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "NodeExpression");
  ntype.ui_name = "Expression";
  ntype.ui_description = "Evaluate an expression on inputs";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_expression_cc
