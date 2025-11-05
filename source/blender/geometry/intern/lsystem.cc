/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "lsystem.hh"

namespace blender::geometry::lsystem {

void apply_rules(const RuleSet &rules, Span<Symbol> symbols, Vector<Symbol> &r_symbols)
{
  for (const Symbol &symbol : symbols) {
    const Rule *rule = rules.lookup(symbol.symbol_id);
    if (rule) {
      r_symbols.extend(rule->replacement);
    }
    else {
      r_symbols.append(symbol);
    }
  }
}

}  // namespace blender::geometry::lsystem
