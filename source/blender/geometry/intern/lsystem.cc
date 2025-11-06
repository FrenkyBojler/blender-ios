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
      case '/':
      case '"':
      case '[':
      case ']': {
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
      i_++;
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

bool LSystem::add_evaluated_symbols(ResourceScope &scope,
                                    Span<SymbolExpr> symbol_exprs,
                                    TurtleStack &turtle_stack,
                                    Vector<Symbol> &r_symbols) const
{
  for (const SymbolExpr &symbol_expr : symbol_exprs) {
    const Symbol symbol = this->eval_symbol_expr(scope, symbol_expr, turtle_stack);
    if (!this->update_turtle_stack(turtle_stack, symbol)) {
      return false;
    }
    r_symbols.append(symbol);
  }
  return true;
}

std::optional<Vector<Symbol>> LSystem::compute_nth_generation(ResourceScope &scope,
                                                              const Turtle &root_turtle,
                                                              const int generations) const
{
  Vector<Symbol> symbols;
  {
    TurtleStack turtle_stack(root_turtle);
    if (!this->add_evaluated_symbols(scope, axiom_, turtle_stack, symbols)) {
      return std::nullopt;
    }
  }

  for (int i = 0; i < generations; i++) {
    Vector<Symbol> new_symbols;
    TurtleStack turtle_stack(root_turtle);
    for (const Symbol &symbol : symbols) {
      if (const Rule *rule = this->lookup_rule(symbol)) {
        if (!this->add_evaluated_symbols(scope, rule->replacement, turtle_stack, new_symbols)) {
          return std::nullopt;
        }
      }
      else {
        new_symbols.append(symbol);
        if (!this->update_turtle_stack(turtle_stack, symbol)) {
          return std::nullopt;
        }
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
                                 const TurtleStack &turtle_stack) const
{
  const Turtle &turtle = turtle_stack.stack.peek();
  switch (symbol_expr.symbol_id) {
    case symbol_F.id:
    case symbol_f.id: {
      return Symbol{
          symbol_expr.symbol_id,
          scope.allocator().construct_array_copy<ParamValue>({{turtle.step}, {turtle.radius}})};
    }
    default: {
      return Symbol{symbol_expr.symbol_id, {}};
    }
  }
}

bool LSystem::update_turtle_stack(TurtleStack &turtle_stack, const Symbol &symbol) const
{
  switch (symbol.symbol_id) {
    case symbol_F.id:
    case symbol_f.id: {
      Turtle &turtle = turtle_stack.stack.peek();
      const float3 offset = math::transform_direction(turtle.orientation,
                                                      float3(0, 0, turtle.step));
      turtle.position += offset;
      break;
    }
    case symbol_branch_start.id: {
      turtle_stack.stack.push(turtle_stack.stack.peek());
      break;
    }
    case symbol_branch_end.id: {
      if (turtle_stack.stack.size() > 1) {
        turtle_stack.stack.pop();
      }
      else {
        return false;
      }
      break;
    }
    default: {
      break;
    }
  }
  return true;
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
