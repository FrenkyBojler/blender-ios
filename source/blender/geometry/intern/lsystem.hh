/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <functional>
#include <optional>
#include <sstream>
#include <variant>

#include <fmt/format.h>

#include "BLI_math_constants.h"
#include "BLI_math_matrix.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_resource_scope.hh"
#include "BLI_stack.hh"
#include "BLI_struct_equality_utils.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

namespace blender::geometry::lsystem {

using SymbolId = int;
using ParamsId = int;

struct ParamValue {
  float value;

  BLI_STRUCT_EQUALITY_OPERATORS_1(ParamValue, value)
};

struct Symbol {
  SymbolId symbol_id;
  Span<ParamValue> params;

  Symbol(SymbolId symbol_id, Span<ParamValue> params) : symbol_id(symbol_id), params(params) {}

  BLI_STRUCT_EQUALITY_OPERATORS_2(Symbol, symbol_id, params)
};

struct ParamExpr {};

struct SymbolExpr {
  SymbolId symbol_id;
  Vector<ParamExpr> params;

  SymbolExpr(SymbolId symbol_id, Span<ParamExpr> params) : symbol_id(symbol_id), params(params) {}
};

struct Rule {
  SymbolId variable_id;
  Vector<SymbolExpr> replacement;
};

struct SymbolIdMap {
  VectorSet<std::string> symbols;

  SymbolId ensure(const StringRef symbol)
  {
    return symbols.index_of_or_add_as(symbol);
  }
};

struct ParamDefaults {
  float step_size = 1.0f;
  float angle = DEG2RAD(90.0f);
  float step_size_scale = 0.5f;
  float default_radius_scale = 0.5f;
  float angle_scale = 0.5f;
};

struct Turtle {
  float3x3 orientation = float3x3::identity();
  float3 position = float3(0.0f, 0.0f, 0.0f);
  float radius = 1.0f;
  float step = 1.0f;
};

struct BuiltinSymbol {
  char name;
  SymbolId id;
};

static constexpr BuiltinSymbol symbol_F{'F', 0};
static constexpr BuiltinSymbol symbol_f{'f', 1};
static constexpr BuiltinSymbol symbol_branch_start{'[', 2};
static constexpr BuiltinSymbol symbol_branch_end{']', 3};

static constexpr std::array builtin_symbols = {
    symbol_F,
    symbol_f,
    symbol_branch_start,
    symbol_branch_end,
};

struct TurtleStack {
  Stack<Turtle> stack;

  TurtleStack(Turtle initial_turtle)
  {
    stack.push(initial_turtle);
  }
};

class LSystem {
 private:
  ResourceScope global_scope_;
  SymbolIdMap symbol_id_map_;
  MultiValueMap<SymbolId, Rule> rules_;
  Vector<SymbolExpr> axiom_;
  ParamDefaults defaults_;

  friend class LSystemBuilder;

 public:
  LSystem();

  bool set_axiom(StringRef axiom_str);
  bool add_rule(StringRef rule_str);

  void set_defaults(ParamDefaults defaults)
  {
    defaults_ = std::move(defaults);
  }

  Span<SymbolExpr> axiom() const
  {
    return axiom_;
  }

  const Rule *lookup_rule(const Symbol &symbol) const
  {
    for (const Rule &rule : rules_.lookup(symbol.symbol_id)) {
      return &rule;
    }
    return nullptr;
  }

  std::optional<Vector<Symbol>> compute_nth_generation(ResourceScope &scope,
                                                       const Turtle &root_turtle,
                                                       const int generations) const;

  Symbol eval_symbol_expr(ResourceScope &scope,
                          const SymbolExpr &symbol_expr,
                          const TurtleStack &turtle_stack) const;

  [[nodiscard]] bool update_turtle_stack(TurtleStack &turtle_stack, const Symbol &symbol) const;

  [[nodiscard]] bool add_evaluated_symbols(ResourceScope &scope,
                                           Span<SymbolExpr> symbol_exprs,
                                           TurtleStack &turtle_stack,
                                           Vector<Symbol> &r_symbols) const;

  std::string symbols_to_string(Span<Symbol> symbols) const;
};

}  // namespace blender::geometry::lsystem
