/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>

#include "NOD_expression_parse.hh"

#include "expression_parse.hh"

namespace blender::nodes::expression {

class Tokenizer {
 private:
  const StringRef full_str_;
  Vector<Token> r_tokens_;

 public:
  Tokenizer(const StringRef full_str, Vector<Token> &r_tokens)
      : full_str_(full_str), r_tokens_(r_tokens)
  {
  }

  void tokenize() {}
};

void tokenize(const StringRef expression, Vector<Token> &r_tokens)
{
  Tokenizer tokenizer(expression, r_tokens);
  tokenizer.tokenize();
}

ast::Expr *parse(ResourceScope &scope, const StringRef expression, std::ostream &r_errors)
{
  r_errors << "Not implemented";
  return nullptr;
}

}  // namespace blender::nodes::expression
