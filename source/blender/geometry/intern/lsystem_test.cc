/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "lsystem.hh"

namespace blender::geometry::lsystem::tests {

TEST(lsystem, ApplyRules)
{
  LSystem lsystem;
  const SymbolId F_id = lsystem.ensure_symbol_id("F");
  const Symbol F{F_id};
  lsystem.add_rule(Rule{F_id, {F, F}});

  Vector<Symbol> new_symbols;
  apply_rules(lsystem, {F}, new_symbols);
  EXPECT_EQ_SPAN<Symbol>(new_symbols, {F, F});
}

}  // namespace blender::geometry::lsystem::tests
