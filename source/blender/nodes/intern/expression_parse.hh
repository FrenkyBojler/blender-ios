/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_ref.hh"

#include "BLI_vector.hh"

namespace blender::nodes::expression {

enum class TokenType {
  Number,
  Identifier,
  String,
  Special,
};

struct Token {
  TokenType type;
  IndexRange range;
};

void tokenize(const StringRef expression, Vector<Token> &r_tokens);

}  // namespace blender::nodes::expression
