/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "lsystem.hh"
#include "BLI_resource_scope.hh"

namespace blender::geometry::lsystem {

class LSystemParser {
 private:
  ResourceScope &scope_;
  StringRef full_str_;
  int64_t i_ = 0;
  SymbolIdMap &symbol_id_map_;

 public:
  LSystemParser(ResourceScope &scope, const StringRef full_str, SymbolIdMap &symbol_id_map)
      : scope_(scope), full_str_(full_str), symbol_id_map_(symbol_id_map)
  {
  }

  std::optional<Vector<SymbolExpr>> parse_symbol_expressions()
  {
    Vector<SymbolExpr> symbols;
    while (const std::optional<SymbolExpr> symbol_expr = this->parse_symbol_expression()) {
      symbols.append(*symbol_expr);
    }
    return symbols;
  }

  std::optional<Rule> parse_rule()
  {
    const std::optional<SymbolId> variable_id = this->parse_variable_id();
    if (!variable_id) {
      return std::nullopt;
    }
    if (!this->consume_next_if('=')) {
      return std::nullopt;
    }
    const std::optional<Vector<SymbolExpr>> replacement = this->parse_symbol_expressions();
    if (!replacement) {
      return std::nullopt;
    }
    this->consume_next_if('\n');
    return Rule{*variable_id, std::move(*replacement)};
  }

  std::optional<SymbolId> parse_variable_id()
  {
    if (this->is_end()) {
      return std::nullopt;
    }
    const char first_c = full_str_[i_];
    switch (first_c) {
      case 'F':
      case 'f':
      case 'A':
      case 'B':
      case 'X':
      case 'Y':
      case 'Z': {
        this->consume_next();
        return symbol_id_map_.ensure(StringRef(&first_c, 1));
      }
    }
    return std::nullopt;
  }

  std::optional<SymbolId> parse_symbol_id()
  {
    if (this->is_end()) {
      return std::nullopt;
    }
    const char first_c = full_str_[i_];
    switch (first_c) {
      case 'F':
      case 'f':
      case 'A':
      case 'B':
      case 'X':
      case 'Y':
      case 'Z':
      case '+':
      case '-':
      case '&':
      case '^':
      case '\\':
      case '/': {
        this->consume_next();
        return symbol_id_map_.ensure(StringRef(&first_c, 1));
      }
    }
    return std::nullopt;
  }

  std::optional<SymbolExpr> parse_symbol_expression()
  {
    const std::optional<SymbolId> symbol_id = this->parse_symbol_id();
    if (!symbol_id) {
      return std::nullopt;
    }
    std::optional<Vector<ParamExpr>> params = this->parse_params();
    if (!params) {
      return std::nullopt;
    }
    return SymbolExpr{*symbol_id, std::move(*params)};
  }

  std::optional<Vector<ParamExpr>> parse_params()
  {
    return Vector<ParamExpr>();
  }

  bool is_end() const
  {
    return i_ >= full_str_.size();
  }

  bool next_is(const char c) const
  {
    if (this->is_end()) {
      return false;
    }
    return full_str_[i_] == c;
  }

  bool consume_next_if(const char c)
  {
    if (this->next_is(c)) {
      this->consume_next();
      return true;
    }
    return false;
  }

  void consume_next()
  {
    i_++;
    this->consume_whitespace();
  }

  void consume_whitespace()
  {
    while (true) {
      if (this->is_end()) {
        break;
      }
      const char c = full_str_[i_];
      if (!ELEM(c, ' ', '\t', '\r')) {
        break;
      }
    }
  }
};

bool LSystem::set_axiom(const StringRef axiom_str)
{
  LSystemParser parser{global_scope_, axiom_str, symbol_id_map_};
  if (const std::optional<Vector<SymbolExpr>> symbols = parser.parse_symbol_expressions()) {
    axiom_ = std::move(*symbols);
    return true;
  }
  return false;
}

bool LSystem::add_rule(StringRef rule_str)
{
  LSystemParser parser{global_scope_, rule_str, symbol_id_map_};
  if (const std::optional<Rule> rule = parser.parse_rule()) {
    rules_.add(rule->variable_id, std::move(*rule));
    return true;
  }
  return false;
}

Vector<Symbol> LSystem::compute_nth_generation(ResourceScope &scope,
                                               const Turtle &root_turtle,
                                               const int generations) const
{
  Vector<Symbol> symbols;
  {
    Turtle turtle = root_turtle;
    for (const SymbolExpr &symbol_expr : axiom_) {
      const Symbol symbol = this->eval_symbol_expr(scope, symbol_expr, turtle);
      this->update_turtle(turtle, symbol);
      symbols.append(symbol);
    }
  }

  for (int i = 0; i < generations; i++) {
    Vector<Symbol> new_symbols;
    Turtle turtle = root_turtle;
    for (const Symbol &symbol : symbols) {
      if (const Rule *rule = this->lookup_rule(symbol)) {
        for (const SymbolExpr &expr : rule->replacement) {
          const Symbol new_symbol = this->eval_symbol_expr(scope, expr, turtle);
          new_symbols.append(new_symbol);
          this->update_turtle(turtle, new_symbol);
        }
      }
      else {
        new_symbols.append(symbol);
        this->update_turtle(turtle, symbol);
      }
    }
    symbols = std::move(new_symbols);
  }
  return symbols;
}

// Vector<Symbol> LSystem::apply_single_generation(const Span<SymbolExpr> symbols) const
// {
//   Vector<Symbol> r_symbols;
//   for (const SymbolExpr &symbol : symbols) {
//     if (const Rule *rule = this->lookup_rule(symbol.symbol_id)) {
//       r_symbols.extend(rule->replacement);
//     }
//     else {
//       r_symbols.append(symbol);
//     }
//   }
//   return r_symbols;
// }

LSystem::LSystem()
{
  for (const BuiltinSymbol &symbol : builtin_symbols) {
    const SymbolId id = symbol_id_map_.ensure(StringRef(&symbol.name, 1));
    BLI_assert(id == symbol.id);
  }
}

Symbol LSystem::eval_symbol_expr(ResourceScope &scope,
                                 const SymbolExpr &symbol_expr,
                                 const Turtle &turtle) const
{
  switch (symbol_expr.symbol_id) {
    case symbol_id_F.id:
    case symbol_id_f.id: {
      return Symbol{
          symbol_expr.symbol_id,
          scope.allocator().construct_array_copy<ParamValue>({{turtle.step}, {turtle.radius}})};
    }
    default: {
      return Symbol{symbol_expr.symbol_id, {}};
    }
  }
}

void LSystem::update_turtle(Turtle &turtle, const Symbol &symbol) const
{
  switch (symbol.symbol_id) {
    case symbol_id_F.id:
    case symbol_id_f.id: {
      const float3 offset = math::transform_direction(turtle.orientation,
                                                      float3(0, 0, turtle.step));
      turtle.position += offset;
      break;
    }
    default: {
      break;
    }
  }
}

std::string LSystem::symbols_to_string(const Span<Symbol> symbols) const
{
  fmt::memory_buffer buffer;
  fmt::appender buf = fmt::appender(buffer);
  for (const Symbol &symbol : symbols) {
    BLI_assert(symbol_id_map_.symbols.index_range().contains(symbol.symbol_id));
    const StringRef name = symbol_id_map_.symbols[symbol.symbol_id];
    fmt::format_to(buf, "{}", name);
  }
  return std::string(buffer.data(), buffer.size());
}

}  // namespace blender::geometry::lsystem
