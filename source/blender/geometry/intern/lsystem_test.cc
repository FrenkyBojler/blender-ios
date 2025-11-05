/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "lsystem.hh"

namespace blender::geometry::lsystem::tests {

TEST(lsystem, ApplyRules)
{
  const int F_id = 'F';
  const Symbol F{F_id};
  RuleSet rules;
  rules.rules.append(Rule{F_id, {F, F}});
  Vector<Symbol> symbols;
  symbols.append(F);

  Vector<Symbol> new_symbols;
  apply_rules(rules, symbols, new_symbols);
  EXPECT_EQ_SPAN<Symbol>(new_symbols, {F, F});
}

}  // namespace blender::geometry::lsystem::tests
