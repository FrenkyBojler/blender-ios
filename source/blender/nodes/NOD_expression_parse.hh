/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "NOD_expression_ast.hh"

#include "BLI_resource_scope.hh"

namespace blender::nodes::expression {

ast::Expr *parse(ResourceScope &scope, StringRef expression, std::ostream &r_errors);

}  // namespace blender::nodes::expression
