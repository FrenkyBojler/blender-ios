/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_resource_scope.hh"

#include "BLT_translation.hh"

#include "GEO_lsystem.hh"

#include "lsystem.hh"

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
      case '/':
      case '"':
      case '[':
      case ']': {
        this->consume_next();
        return symbol_id_map_.ensure(StringRef(&first_c, 1));
      }
      case '\\': {
        this->consume_next_only();
        if (this->consume_next_if('\\')) {
          return symbol_id_map_.ensure("\\\\");
        }
        return std::nullopt;
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

  void consume_next_only()
  {
    i_++;
  }

  void consume_next()
  {
    this->consume_next_only();
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
      this->consume_next_only();
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

LSystem::LSystem()
{
  for (const BuiltinSymbol &symbol : builtin_symbols) {
    const SymbolId id = symbol_id_map_.ensure(symbol.name);
    BLI_assert(id == symbol.id);
  }
}

Symbol LSystem::eval_symbol_expr(ResourceScope &scope,
                                 const SymbolExpr &symbol_expr,
                                 const TurtleStack &turtle_stack) const
{
  const Turtle &turtle = turtle_stack.peek();
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

static void update_turtle_F(Turtle &turtle, const Symbol & /*symbol*/)
{
  const float3 offset = math::transform_direction(turtle.orientation, float3(0, 0, turtle.step));
  turtle.position += offset;
}

static void update_turtle_f(Turtle &turtle, const Symbol &symbol)
{
  update_turtle_F(turtle, symbol);
}

template<math::AxisSigned::Value Axis>
static void update_turtle_rotation(Turtle &turtle, const float angle)
{
  const float3x3 rotation = math::from_rotation<float3x3>(math::AxisAngle(Axis, angle));
  turtle.orientation = turtle.orientation * rotation;
}

static void update_turtle_rotate_symbol(Turtle &turtle, const Symbol &symbol)
{
  switch (symbol.symbol_id) {
    case symbol_plus.id: {
      update_turtle_rotation<math::AxisSigned::X_POS>(turtle, turtle.angle);
      break;
    }
    case symbol_minus.id: {
      update_turtle_rotation<math::AxisSigned::X_NEG>(turtle, turtle.angle);
      break;
    }
    case symbol_ampersand.id: {
      update_turtle_rotation<math::AxisSigned::Y_POS>(turtle, turtle.angle);
      break;
    }
    case symbol_carret.id: {
      update_turtle_rotation<math::AxisSigned::Y_NEG>(turtle, turtle.angle);
      break;
    }
    case symbol_backslash.id: {
      update_turtle_rotation<math::AxisSigned::Z_POS>(turtle, turtle.angle);
      break;
    }
    case symbol_slash.id: {
      update_turtle_rotation<math::AxisSigned::Z_NEG>(turtle, turtle.angle);
      break;
    }
  }
}

bool LSystem::update_turtle_stack(TurtleStack &turtle_stack, const Symbol &symbol) const
{
  switch (symbol.symbol_id) {
    case symbol_F.id: {
      update_turtle_F(turtle_stack.peek(), symbol);
      break;
    }
    case symbol_f.id: {
      update_turtle_f(turtle_stack.peek(), symbol);
      break;
    }
    case symbol_plus.id:
    case symbol_minus.id:
    case symbol_ampersand.id:
    case symbol_carret.id:
    case symbol_backslash.id:
    case symbol_slash.id: {
      update_turtle_rotate_symbol(turtle_stack.peek(), symbol);
      break;
    }
    case symbol_branch_start.id: {
      turtle_stack.stack.push(turtle_stack.peek());
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

std::variant<bke::CurvesGeometry, std::string> lsystem_to_curves(LSystemParams &params)
{
  ResourceScope scope;
  LSystem lsystem;
  lsystem.set_axiom(params.axiom);
  for (const StringRef rule : params.rules) {
    lsystem.add_rule(rule);
  }

  Turtle root_turtle;
  root_turtle.angle = params.angle;
  const std::optional<Vector<Symbol>> symbols = lsystem.compute_nth_generation(
      scope, root_turtle, params.generations);
  if (!symbols) {
    return TIP_("Failed to generate lsystem result.");
  }

  Vector<Vector<float3>> gathered_curve_points;
  Stack<Vector<float3>> current_curves;
  current_curves.push(Vector<float3>());

  TurtleStack stack(root_turtle);
  for (const Symbol &symbol : *symbols) {
    switch (symbol.symbol_id) {
      case symbol_F.id: {
        Turtle &turtle = stack.peek();
        Vector<float3> &current_curve = current_curves.peek();
        if (current_curve.is_empty()) {
          current_curve.append(turtle.position);
        }
        update_turtle_F(turtle, symbol);
        current_curve.append(turtle.position);
        break;
      }
      case symbol_f.id: {
        Vector<float3> &current_curve = current_curves.peek();
        if (!current_curve.is_empty()) {
          gathered_curve_points.append(std::move(current_curve));
        }
        Turtle &turtle = stack.peek();
        update_turtle_f(turtle, symbol);
        break;
      }
      case symbol_plus.id:
      case symbol_minus.id:
      case symbol_ampersand.id:
      case symbol_carret.id:
      case symbol_backslash.id:
      case symbol_slash.id: {
        update_turtle_rotate_symbol(stack.peek(), symbol);
        break;
      }
      case symbol_branch_start.id: {
        stack.stack.push(stack.peek());
        current_curves.push(Vector<float3>());
        break;
      }
      case symbol_branch_end.id: {
        stack.stack.pop();
        if (stack.stack.is_empty()) {
          return TIP_("More branches are closed than opened.");
        }
        Vector<float3> curve = current_curves.pop();
        if (!curve.is_empty()) {
          gathered_curve_points.append(std::move(curve));
        }
        break;
      }
    }
  }

  while (!stack.stack.is_empty()) {
    stack.stack.pop();
    Vector<float3> curve = current_curves.pop();
    if (!curve.is_empty()) {
      gathered_curve_points.append(std::move(curve));
    }
  }

  int curves_num = gathered_curve_points.size();
  int points_num = 0;
  for (const Span<float3> curve : gathered_curve_points) {
    points_num += curve.size();
  }

  bke::CurvesGeometry curves(points_num, curves_num);
  if (curves_num == 0) {
    return curves;
  }

  MutableSpan<int> offsets = curves.offsets_for_write();
  for (const int i : gathered_curve_points.index_range()) {
    const Span<float3> curve = gathered_curve_points[i];
    offsets[i] = curve.size();
  }
  offset_indices::accumulate_counts_to_offsets(offsets);
  OffsetIndices<int> points_by_curve = curves.points_by_curve();

  MutableSpan<float3> positions = curves.positions_for_write();
  threading::parallel_for(gathered_curve_points.index_range(), 512, [&](const IndexRange range) {
    for (const int curve_i : range) {
      const IndexRange points = points_by_curve[curve_i];
      const Span<float3> curve = gathered_curve_points[curve_i];
      positions.slice(points).copy_from(curve);
    }
  });

  curves.fill_curve_types(CURVE_TYPE_POLY);
  return curves;
}

}  // namespace blender::geometry::lsystem
