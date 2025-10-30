/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"

#include "expression_parse.hh"

namespace blender::nodes::expression::tests {

static void expect_tokens(const TokenizeResult &result, const Span<StringRef> expected_tokens)
{
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

TEST(expression_parse, tokenize_empty)
{
  expect_tokens(tokenize(""), {});
  expect_tokens(tokenize(" "), {});
  expect_tokens(tokenize("  \n\n\t\n\t\t \n"), {});
}

TEST(expression_parse, tokenize_identifier)
{
  expect_tokens(tokenize("a"), {"a"});
  expect_tokens(tokenize("abc qwe"), {"abc", "qwe"});
  expect_tokens(tokenize("abc\nqwe34 \n"), {"abc", "qwe34"});
}

TEST(expression_parse, tokenize_number)
{
  expect_tokens(tokenize("0"), {"0"});
  expect_tokens(tokenize("123"), {"123"});
  expect_tokens(tokenize("123 634"), {"123", "634"});
  expect_tokens(tokenize("123.456"), {"123.456"});
  expect_tokens(tokenize("123.456."), {"123.456", "."});
  expect_tokens(tokenize("123..."), {"123.", ".", "."});
}

TEST(expression_parse, tokenize_string)
{
  expect_tokens(tokenize("\"abc\""), {"\"abc\""});
  expect_tokens(tokenize("\"abc\n'qwe34 \n\" \"\""), {"\"abc\n'qwe34 \n\"", "\"\""});
}

TEST(expression_parse, tokenize_string_unterminated)
{
  const TokenizeResult result = tokenize("\"abc");
  const StringRef error = std::get<std::string>(result.result);
  EXPECT_TRUE(error.startswith("Unterminated string"));
}

TEST(expression_parse, tokenize_special)
{
  expect_tokens(tokenize("+"), {"+"});
  expect_tokens(tokenize("-"), {"-"});
  expect_tokens(tokenize("*+"), {"*", "+"});
  expect_tokens(tokenize("<="), {"<="});
  expect_tokens(tokenize("< ="), {"<", "="});
  expect_tokens(tokenize("=="), {"=="});
  expect_tokens(tokenize(">>>"), {">>", ">"});
}

TEST(expression_parse, invalid_char)
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

}  // namespace blender::nodes::expression::tests
