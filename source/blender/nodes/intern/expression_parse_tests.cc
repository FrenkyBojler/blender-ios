/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"

#include "expression_parse.hh"

namespace blender::nodes::expression::tests {

TEST(expression_parse, tokenize_empty)
{
  {
    Vector<Token> tokens;
    tokenize("", tokens);
    EXPECT_TRUE(tokens.is_empty());
  }
  {
    Vector<Token> tokens;
    tokenize(" ", tokens);
    EXPECT_TRUE(tokens.is_empty());
  }
  {
    Vector<Token> tokens;
    tokenize("  \n\n\t\n\t\t \n", tokens);
    EXPECT_TRUE(tokens.is_empty());
  }
}

}  // namespace blender::nodes::expression::tests
