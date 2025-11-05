/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "lsystem.hh"

namespace blender::geometry::lsystem {

class LSystemParser {
 private:
  StringRef full_str_;
  int64_t i_ = 0;
  SymbolIdMap &symbol_id_map_;
  SymbolParamsVector &params_vector_;

 public:
  LSystemParser(const StringRef full_str,
                SymbolIdMap &symbol_id_map,
                SymbolParamsVector &params_vector)
      : full_str_(full_str), symbol_id_map_(symbol_id_map), params_vector_(params_vector)
  {
  }

  std::optional<Vector<Symbol>> parse_symbols()
  {
    Vector<Symbol> symbols;
    while (const std::optional<Symbol> symbol = this->parse_symbol()) {
      symbols.append(*symbol);
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
    const std::optional<Vector<Symbol>> replacement = this->parse_symbols();
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

  std::optional<Symbol> parse_symbol()
  {
    const std::optional<SymbolId> variable_id = this->parse_variable_id();
    if (!variable_id) {
      if (this->is_end()) {
        return std::nullopt;
      }
      const char first_c = full_str_[i_];
      switch (first_c) {
        case '+':
        case '-':
        case '&':
        case '^':
        case '\\':
        case '/': {
          this->consume_next();
          const SymbolId id = symbol_id_map_.ensure(StringRef(&first_c, 1));
          if (const std::optional<ParamsId> params_id = this->parse_params_id_angle()) {
            return Symbol{id, *params_id};
          }
          return std::nullopt;
        }
      }
      return std::nullopt;
    }
    if (!this->next_is('(')) {
      return Symbol{*variable_id, -1};
    }
    const StringRef name = symbol_id_map_.symbols[*variable_id];
    if (name.size() == 1) {
      const char first_c = name[0];
      switch (first_c) {
        case 'F': {
          this->consume_next();
          const SymbolId id = symbol_id_map_.ensure("F");
          if (const std::optional<ParamsId> params_id = this->parse_params_id_F()) {
            return Symbol{id, *params_id};
          }
          return std::nullopt;
        }
        case 'f': {
          this->consume_next();
          const SymbolId id = symbol_id_map_.ensure("f");
          if (const std::optional<ParamsId> params_id = this->parse_params_id_f()) {
            return Symbol{id, *params_id};
          }
          return std::nullopt;
        }
      }
    }

    return {};
  }

  std::optional<ParamsId> parse_params_id_F()
  {
    if (std::optional<Params_F> params = this->parse_params_F()) {
      return params_vector_.add(*params);
    }
    return {};
  }

  std::optional<Params_F> parse_params_F()
  {
    if (!this->next_is('(')) {
      return Params_F{};
    }
    /* TODO: Parse explicit args. */
    return std::nullopt;
  }

  std::optional<ParamsId> parse_params_id_f()
  {
    if (std::optional<Params_f> params = this->parse_params_f()) {
      return params_vector_.add(*params);
    }
    return {};
  }

  std::optional<Params_f> parse_params_f()
  {
    if (!this->next_is('(')) {
      return Params_f{};
    }
    /* TODO: Parse explicit args. */
    return std::nullopt;
  }

  std::optional<ParamsId> parse_params_id_angle()
  {
    if (std::optional<Params_Angle> params = this->parse_params_angle()) {
      return params_vector_.add(*params);
    }
    return {};
  }

  std::optional<Params_Angle> parse_params_angle()
  {
    if (!this->next_is('(')) {
      return Params_Angle{};
    }
    /* TODO: Parse explicit args. */
    return std::nullopt;
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

bool LSystemBuilder::set_axiom(const StringRef axiom_str)
{
  LSystemParser parser{axiom_str, lsystem_.symbol_id_map_, lsystem_.params_vector_};
  if (const std::optional<Vector<Symbol>> symbols = parser.parse_symbols()) {
    lsystem_.axiom_ = std::move(*symbols);
    return true;
  }
  return false;
}

bool LSystemBuilder::add_rule(StringRef rule_str)
{
  LSystemParser parser{rule_str, lsystem_.symbol_id_map_, lsystem_.params_vector_};
  if (const std::optional<Rule> rule = parser.parse_rule()) {
    lsystem_.rules_.add(rule->variable_id, std::move(*rule));
    return true;
  }
  return false;
}

LSystem LSystemBuilder::build()
{
  return std::move(lsystem_);
}

Vector<Symbol> LSystem::compute_nth_generation(const int generations) const
{
  Vector<Symbol> symbols = this->axiom();
  for (int i = 0; i < generations; i++) {
    symbols = this->apply_single_generation(symbols);
  }
  return symbols;
}

Vector<Symbol> LSystem::apply_single_generation(const Span<Symbol> symbols) const
{
  Vector<Symbol> r_symbols;
  for (const Symbol &symbol : symbols) {
    if (const Rule *rule = this->lookup_rule(symbol.symbol_id)) {
      r_symbols.extend(rule->replacement);
    }
    else {
      r_symbols.append(symbol);
    }
  }
  return r_symbols;
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
