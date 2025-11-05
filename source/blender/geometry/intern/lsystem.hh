/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <functional>
#include <optional>
#include <sstream>
#include <variant>

#include <fmt/format.h>

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

  Symbol(SymbolId symbol_id, ParamsId params_id) : symbol_id(symbol_id), params_id(params_id) {}

  BLI_STRUCT_EQUALITY_OPERATORS_2(Symbol, symbol_id, params_id)
};

struct Params_F {
  std::optional<float> distance;

  BLI_STRUCT_EQUALITY_OPERATORS_1(Params_F, distance)
};

struct Params_f {
  std::optional<float> distance;

  BLI_STRUCT_EQUALITY_OPERATORS_1(Params_f, distance)
};

struct Params_T {
  std::optional<float> strength;

  BLI_STRUCT_EQUALITY_OPERATORS_1(Params_T, strength)
};

struct Params_Angle {
  std::optional<float> angle;

  BLI_STRUCT_EQUALITY_OPERATORS_1(Params_Angle, angle)
};

using SymbolParams = std::variant<Params_F, Params_f, Params_T, Params_Angle>;

struct Rule {
  SymbolId variable_id;
  Vector<Symbol> replacement;
};

struct SymbolIdMap {
  VectorSet<std::string> symbols;

  SymbolId ensure(const StringRef symbol)
  {
    return symbols.index_of_or_add_as(symbol);
  }
};

struct SymbolParamsVector {
  Vector<SymbolParams> params;

  ParamsId add(SymbolParams params)
  {
    return this->params.append_and_get_index(std::move(params));
  }
};

class LSystem {
 private:
  SymbolIdMap symbol_id_map_;
  SymbolParamsVector params_vector_;
  MultiValueMap<SymbolId, Rule> rules_;
  Vector<Symbol> axiom_;

  friend class LSystemBuilder;

 public:
  Span<Symbol> axiom() const
  {
    return axiom_;
  }

  const Rule *lookup_rule(const SymbolId symbol_id) const
  {
    for (const Rule &rule : rules_.lookup(symbol_id)) {
      return &rule;
    }
    return nullptr;
  }

  Vector<Symbol> compute_nth_generation(const int generations) const;
  Vector<Symbol> apply_single_generation(Span<Symbol> symbols) const;

  std::string symbols_to_string(Span<Symbol> symbols) const;
};

class LSystemBuilder {
 private:
  LSystem lsystem_;

 public:
  bool set_axiom(StringRef axiom_str);
  bool add_rule(StringRef rule_str);

  LSystem build();
};

}  // namespace blender::geometry::lsystem
