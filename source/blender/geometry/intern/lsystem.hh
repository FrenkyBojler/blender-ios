/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>
#include <variant>

#include "BLI_multi_value_map.hh"
#include "BLI_struct_equality_utils.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

namespace blender::geometry::lsystem {

using SymbolId = int;
using ParamsId = int;

struct Symbol {
  SymbolId symbol_id;
  ParamsId params_id;

  Symbol(SymbolId symbol_id, ParamsId params_id = -1) : symbol_id(symbol_id), params_id(params_id)
  {
  }

  BLI_STRUCT_EQUALITY_OPERATORS_2(Symbol, symbol_id, params_id)
};

struct Params_F {
  std::optional<float> distance;
};

struct Params_f {
  std::optional<float> distance;
};

struct Params_T {
  std::optional<float> strength;
};

struct Params_Angle {
  std::optional<float> angle;
};

using ParamsVariant = std::variant<Params_F, Params_f, Params_T, Params_Angle>;

struct Rule {
  SymbolId symbol_id;
  Vector<Symbol> replacement;
};

class LSystem {
 private:
  VectorSet<std::string> symbols_;
  Vector<ParamsVariant> params_;
  MultiValueMap<SymbolId, Rule> rules_;

 public:
  SymbolId ensure_symbol_id(const StringRef symbol)
  {
    return symbols_.index_of_or_add_as(symbol);
  }

  ParamsId add_params(ParamsVariant params)
  {
    return params_.append_and_get_index(std::move(params));
  }

  void add_rule(Rule rule)
  {
    rules_.add(rule.symbol_id, std::move(rule));
  }

  const Rule *lookup_rule(SymbolId symbol_id) const
  {
    for (const Rule &rule : rules_.lookup(symbol_id)) {
      return &rule;
    }
    return nullptr;
  }
};

void apply_rules(const LSystem &lsystem, Span<Symbol> symbols, Vector<Symbol> &r_symbols);

}  // namespace blender::geometry::lsystem
