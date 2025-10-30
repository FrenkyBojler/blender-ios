/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>

#include "NOD_expression_parse.hh"

namespace blender::nodes::expression {

ast::Expr *parse(ResourceScope &scope, const StringRef expression, std::ostream &r_errors)
{
  r_errors << "Not implemented";
  return nullptr;
}

}  // namespace blender::nodes::expression
