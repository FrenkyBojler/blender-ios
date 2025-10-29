/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <ctpg/ctpg.hpp>

#include "NOD_expression_parse.hh"

namespace blender::nodes::expression {

class ParseContext {
 public:
  ResourceScope &scope;
};

namespace grammar {

using namespace ctpg;

constexpr nterm<ast::Expr *> expr("expr");
constexpr nterm<Vector<ast::Expr *>> expr_list("args");

constexpr char number_pattern[] = "([1-9][0-9]*|0)(\\.[0-9]+)?";
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
    terms(op_plus,
          op_minus,
          op_multiply,
          op_divide,
          number,
          identifier,
          '(',
          ')',
          ',',
          op_member_access),
    nterms(expr, expr_list),
    rules(
        expr(number) >>=
        [](ParseContext &ctx, const term_value<std::string_view> &v_number) {
          return &ctx.scope.construct<ast::Expr>(ast::NumberLiteral{v_number.get_value()});
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
          return &ctx.scope.construct<ast::Expr>(
              ast::MemberAccess{v_expr, StringRef(v_identifier)});
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
        expr_list() >>= [](ParseContext & /*ctx*/) { return Vector<ast::Expr *>{}; },
        expr_list(expr) >>= [](ParseContext & /*ctx*/,
                               ast::Expr *v_expr) { return Vector<ast::Expr *>{v_expr}; },
        expr_list(expr_list, ',', expr) >>=
        [](ParseContext & /*ctx*/, Vector<ast::Expr *> v_expr_list, char /*skip*/, ast::Expr *b) {
          v_expr_list.append(b);
          return v_expr_list;
        },
        expr(identifier, '(', expr_list, ')') >>=
        [](ParseContext &ctx,
           const term_value<std::string_view> &v_identifier,
           char /*skip*/,
           Vector<ast::Expr *> v_args,
           char /*skip*/) {
          return &ctx.scope.construct<ast::Expr>(
              ast::Call{StringRef(v_identifier), std::move(v_args)});
        }

        ));

}  // namespace grammar

ast::Expr *parse(ResourceScope &scope, StringRef expression, std::ostream &r_errors)
{
  ParseContext ctx{scope};
  auto result = grammar::p.context_parse(
      ctx, ctpg::buffers::string_view_buffer(expression), r_errors);
  if (!result.has_value()) {
    return nullptr;
  }
  return *result;
}

}  // namespace blender::nodes::expression
