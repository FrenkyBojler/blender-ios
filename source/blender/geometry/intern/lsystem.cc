/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "lsystem.hh"

namespace blender::geometry::lsystem {

void apply_rules(const LSystem &lsystem, Span<Symbol> symbols, Vector<Symbol> &r_symbols)
{
  for (const Symbol &symbol : symbols) {
    if (const Rule *rule = lsystem.lookup_rule(symbol.symbol_id)) {
      r_symbols.extend(rule->replacement);
    }
    else {
      r_symbols.append(symbol);
    }
  }
}

}  // namespace blender::geometry::lsystem
