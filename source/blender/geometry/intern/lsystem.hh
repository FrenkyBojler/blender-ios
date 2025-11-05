/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_struct_equality_utils.hh"
#include "BLI_vector.hh"

namespace blender::geometry::lsystem {

struct Symbol {
  int symbol_id;
  int params_id;

  Symbol(int symbol_id, int params_id = -1) : symbol_id(symbol_id), params_id(params_id) {}

  BLI_STRUCT_EQUALITY_OPERATORS_2(Symbol, symbol_id, params_id)
};

struct Rule {
  int symbol_id;
  Vector<Symbol> replacement;
};

struct RuleSet {
  Vector<Rule> rules;

  const Rule *lookup(int symbol_id) const
  {
    for (const Rule &rule : rules) {
      if (rule.symbol_id == symbol_id) {
        return &rule;
      }
    }
    return nullptr;
  }
};

void apply_rules(const RuleSet &rules, Span<Symbol> symbols, Vector<Symbol> &r_symbols);

}  // namespace blender::geometry::lsystem
