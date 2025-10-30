/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "NOD_expression_parse.hh"

#include "expression_parse.hh"

namespace blender::nodes::expression::tests {

static void expect_tokens(const StringRef src, const Span<StringRef> expected_tokens)
{
  const TokenizeResult result = tokenize(src);
  const Vector<Token> *found_tokens = std::get_if<Vector<Token>>(&result.result);
  if (!found_tokens) {
    FAIL() << "Expected tokens, got error: " << std::get<std::string>(result.result);
    return;
  }
  EXPECT_EQ(found_tokens->size(), expected_tokens.size());
  for (const int i : expected_tokens.index_range()) {
    const StringRef expected_str = expected_tokens[i];
    const Token &found = (*found_tokens)[i];
    EXPECT_EQ(found.str, expected_str);
  }
}

TEST(nodes_expression, tokenize_empty)
{
  expect_tokens("", {});
  expect_tokens(" ", {});
  expect_tokens("  \n\n\t\n\t\t \n", {});
}

TEST(nodes_expression, tokenize_identifier)
{
  expect_tokens("a", {"a"});
  expect_tokens("abc qwe", {"abc", "qwe"});
  expect_tokens("abc\nqwe34 \n", {"abc", "qwe34"});
}

TEST(nodes_expression, tokenize_number)
{
  expect_tokens("0", {"0"});
  expect_tokens("123", {"123"});
  expect_tokens("123 634", {"123", "634"});
  expect_tokens("123.456", {"123.456"});
  expect_tokens("123.456.", {"123.456", "."});
  expect_tokens("123...", {"123.", ".", "."});
}

TEST(nodes_expression, tokenize_string)
{
  expect_tokens("\"abc\"", {"\"abc\""});
  expect_tokens("\"abc\n'qwe34 \n\" \"\"", {"\"abc\n'qwe34 \n\"", "\"\""});
}

TEST(nodes_expression, tokenize_string_unterminated)
{
  const TokenizeResult result = tokenize("\"abc");
  const StringRef error = std::get<std::string>(result.result);
  EXPECT_TRUE(error.startswith("Unterminated string"));
}

TEST(nodes_expression, tokenize_special)
{
  expect_tokens("+", {"+"});
  expect_tokens("-", {"-"});
  expect_tokens("*+", {"*", "+"});
  expect_tokens("<=", {"<="});
  expect_tokens("< =", {"<", "="});
  expect_tokens("==", {"=="});
  expect_tokens(">>>", {">>", ">"});
}

TEST(nodes_expression, tokenize_invalid_char)
{
  {
    const TokenizeResult result = tokenize("a\x1b");
    const StringRef error = std::get<std::string>(result.result);
    EXPECT_TRUE(error.startswith("Invalid character"));
  }
  {
    const TokenizeResult result = tokenize("a`");
    const StringRef error = std::get<std::string>(result.result);
    EXPECT_TRUE(error.startswith("Invalid character"));
  }
}

static void expect_ast_recursive(const ast::Expr &a, const ast::Expr &b)
{
  EXPECT_EQ(a.expr.index(), b.expr.index());
  if (const auto *a_ = std::get_if<ast::Identifier>(&a.expr)) {
    const auto *b_ = std::get_if<ast::Identifier>(&b.expr);
    EXPECT_EQ(a_->identifier, b_->identifier);
  }
  else if (const auto *a_ = std::get_if<ast::NumberLiteral>(&a.expr)) {
    const auto *b_ = std::get_if<ast::NumberLiteral>(&b.expr);
    EXPECT_EQ(a_->value, b_->value);
  }
  else if (const auto *a_ = std::get_if<ast::StringLiteral>(&a.expr)) {
    const auto *b_ = std::get_if<ast::StringLiteral>(&b.expr);
    EXPECT_EQ(a_->value, b_->value);
  }
  else if (const auto *a_ = std::get_if<ast::BinaryOp>(&a.expr)) {
    const auto *b_ = std::get_if<ast::BinaryOp>(&b.expr);
    EXPECT_EQ(a_->op, b_->op);
    expect_ast_recursive(*a_->a, *b_->a);
    expect_ast_recursive(*a_->b, *b_->b);
  }
  else if (const auto *a_ = std::get_if<ast::UnaryOp>(&a.expr)) {
    const auto *b_ = std::get_if<ast::UnaryOp>(&b.expr);
    EXPECT_EQ(a_->op, b_->op);
    expect_ast_recursive(*a_->expr, *b_->expr);
  }
  else if (const auto *a_ = std::get_if<ast::ConditionalOp>(&a.expr)) {
    const auto *b_ = std::get_if<ast::ConditionalOp>(&b.expr);
    expect_ast_recursive(*a_->condition, *b_->condition);
    expect_ast_recursive(*a_->true_expr, *b_->true_expr);
    expect_ast_recursive(*a_->false_expr, *b_->false_expr);
  }
  else if (const auto *a_ = std::get_if<ast::Call>(&a.expr)) {
    const auto *b_ = std::get_if<ast::Call>(&b.expr);
    EXPECT_EQ(a_->function->expr.index(), b_->function->expr.index());
    EXPECT_EQ(a_->args.size(), b_->args.size());
    for (const int i : IndexRange(a_->args.size())) {
      expect_ast_recursive(*a_->args[i], *b_->args[i]);
    }
  }
  else {
    BLI_assert_unreachable();
  }
}

static void expect_ast(const StringRef src, const ast::Expr &b)
{
  ResourceScope scope;
  ParseResult result = parse(scope, src);
  ast::Expr *expr = std::get<ast::Expr *>(result);
  expect_ast_recursive(*expr, b);
}

TEST(nodes_expression, parse_identifier)
{
  expect_ast("a", {ast::Identifier{"a"}});
}

TEST(nodes_expression, parse_number)
{
  expect_ast("123", {ast::NumberLiteral{"123"}});
}

TEST(nodes_expression, parse_string)
{
  expect_ast("\"abc\"", {ast::StringLiteral{"\"abc\""}});
}

}  // namespace blender::nodes::expression::tests
