/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include <cmath>

#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"

#include "MEM_guardedalloc.h"

#include "FN_multi_function.hh"
#include "FN_multi_function_builder.hh"

#include "NOD_multi_function.hh"

#include "GPU_material.hh"

#include "UI_resources.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_expression_cc {

NODE_STORAGE_FUNCS(NodeCMPExpression)

static void cmp_node_expression_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("a").default_value(0.0f);
  b.add_input<decl::Float>("b").default_value(0.0f);
  b.add_input<decl::Float>("c").default_value(0.0f);
  b.add_output<decl::Float>("Result");
}

static void node_composit_init_expression(bNodeTree * /*ntree*/, bNode *node)
{
  NodeCMPExpression *data = MEM_callocN<NodeCMPExpression>(__func__);
  data->expression = BLI_strdup("a + b");
  node->storage = data;
}

static void node_free_expression(bNode *node)
{
  NodeCMPExpression *data = (NodeCMPExpression *)node->storage;
  if (data) {
    if (data->expression) {
      MEM_freeN(data->expression);
    }
    MEM_freeN(data);
  }
}

static void node_copy_expression(bNodeTree * /*dst_ntree*/,
                                 bNode *dest_node,
                                 const bNode *src_node)
{
  const NodeCMPExpression *src_data = (NodeCMPExpression *)src_node->storage;
  NodeCMPExpression *dest_data = (NodeCMPExpression *)MEM_dupallocN(src_data);
  dest_data->expression = BLI_strdup_null(src_data->expression);
  dest_node->storage = dest_data;
}

static void node_composit_buts_expression(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "expression", UI_ITEM_NONE, "", ICON_NONE);
}

using namespace blender::compositor;

class ExpressionEvaluator {
 private:
  const char *expr_;
  size_t pos_;
  const float *vars_;
  int num_vars_;

  char peek() const
  {
    return expr_[pos_];
  }

  char get()
  {
    return expr_[pos_++];
  }

  void skip_whitespace()
  {
    while (peek() == ' ' || peek() == '\t') {
      pos_++;
    }
  }

  float parse_number()
  {
    skip_whitespace();
    float result = 0.0f;
    float decimal = 0.0f;
    int decimal_places = 0;
    bool negative = false;

    if (peek() == '-') {
      negative = true;
      pos_++;
    }

    while (peek() >= '0' && peek() <= '9') {
      result = result * 10.0f + (get() - '0');
    }

    if (peek() == '.') {
      pos_++;
      while (peek() >= '0' && peek() <= '9') {
        decimal = decimal * 10.0f + (get() - '0');
        decimal_places++;
      }
    }

    result += decimal / powf(10.0f, float(decimal_places));
    return negative ? -result : result;
  }

  bool match_string(const char *str)
  {
    size_t i = 0;
    while (str[i] != '\0') {
      if (expr_[pos_ + i] != str[i]) {
        return false;
      }
      i++;
    }
    char next = expr_[pos_ + i];
    if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
        (next >= '0' && next <= '9') || next == '.')
    {
      return false;
    }
    pos_ += i;
    return true;
  }

  float parse_variable()
  {
    skip_whitespace();
    size_t start_pos = pos_;

    if (match_string("pi")) {
      return float(M_PI);
    }
    pos_ = start_pos;

    if (peek() == '\xCF' && expr_[pos_ + 1] == '\x80') {
      pos_ += 2;
      return float(M_PI);
    }

    char c = get();
    switch (c) {
      case 'a':
        return num_vars_ > 0 ? vars_[0] : 0.0f;
      case 'b':
        return num_vars_ > 1 ? vars_[1] : 0.0f;
      case 'c':
        return num_vars_ > 2 ? vars_[2] : 0.0f;
      case 'e':
        return float(M_E);
      default:
        return 0.0f;
    }
  }

  float parse_function()
  {
    skip_whitespace();
    size_t start_pos = pos_;

    if (match_string("smoothmax")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float k = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        float h = max_ff(k - fabsf(a - b), 0.0f);
        return max_ff(a, b) + h * h * 0.25f / k;
      }
    }
    pos_ = start_pos;

    if (match_string("smoothmin")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float k = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        float h = max_ff(k - fabsf(a - b), 0.0f);
        return min_ff(a, b) - h * h * 0.25f / k;
      }
    }
    pos_ = start_pos;

    if (match_string("clamp")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float min_val = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float max_val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return clamp_f(val, min_val, max_val);
      }
    }
    pos_ = start_pos;

    if (match_string("mix")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float t = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return interpf(b, a, t);
      }
    }
    pos_ = start_pos;

    if (match_string("min")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return min_ff(a, b);
      }
    }
    pos_ = start_pos;

    if (match_string("max")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float a = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float b = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return max_ff(a, b);
      }
    }
    pos_ = start_pos;

    if (match_string("pow")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float base = parse_ternary();
        skip_whitespace();
        if (peek() == ',') {
          pos_++;
        }
        float exp = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return powf(base, exp);
      }
    }
    pos_ = start_pos;

    if (match_string("sqrt")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return sqrtf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("abs")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return fabsf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("floor")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return floorf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("ceil")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return ceilf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("sin")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return sinf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("cos")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return cosf(val);
      }
    }
    pos_ = start_pos;

    if (match_string("tan")) {
      skip_whitespace();
      if (peek() == '(') {
        pos_++;
        float val = parse_ternary();
        skip_whitespace();
        if (peek() == ')') {
          pos_++;
        }
        return tanf(val);
      }
    }
    pos_ = start_pos;

    return parse_variable();
  }

  float parse_factor()
  {
    skip_whitespace();
    char c = peek();

    if (c == '(') {
      pos_++;
      float result = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      return result;
    }

    if ((c >= '0' && c <= '9') || c == '-' || c == '.') {
      return parse_number();
    }

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      return parse_function();
    }

    return 0.0f;
  }

  bool is_factor_start()
  {
    skip_whitespace();
    char c = peek();
    return (c >= '0' && c <= '9') || c == '(' || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '\xCF';
  }

  float parse_power()
  {
    float result = parse_factor();

    while (true) {
      skip_whitespace();
      if (peek() == '^') {
        pos_++;
        result = powf(result, parse_factor());
      }
      else if (is_factor_start()) {
        result *= parse_factor();
      }
      else {
        break;
      }
    }

    return result;
  }

  float parse_term()
  {
    float result = parse_power();

    while (true) {
      skip_whitespace();
      char op = peek();

      if (op == '*') {
        pos_++;
        result *= parse_power();
      }
      else if (op == '/') {
        pos_++;
        float divisor = parse_power();
        result = divisor != 0.0f ? result / divisor : 0.0f;
      }
      else if (op == '%') {
        pos_++;
        float divisor = parse_power();
        result = divisor != 0.0f ? fmodf(result, divisor) : 0.0f;
      }
      else {
        break;
      }
    }

    return result;
  }

  float parse_expression()
  {
    float result = parse_term();

    while (true) {
      skip_whitespace();
      char op = peek();

      if (op == '+') {
        pos_++;
        result += parse_term();
      }
      else if (op == '-') {
        pos_++;
        result -= parse_term();
      }
      else {
        break;
      }
    }

    return result;
  }

  float parse_comparison()
  {
    float result = parse_expression();

    while (true) {
      skip_whitespace();
      char op = peek();

      if (op == '<') {
        pos_++;
        if (peek() == '=') {
          pos_++;
          result = (result <= parse_expression()) ? 1.0f : 0.0f;
        }
        else {
          result = (result < parse_expression()) ? 1.0f : 0.0f;
        }
      }
      else if (op == '>') {
        pos_++;
        if (peek() == '=') {
          pos_++;
          result = (result >= parse_expression()) ? 1.0f : 0.0f;
        }
        else {
          result = (result > parse_expression()) ? 1.0f : 0.0f;
        }
      }
      else if (op == '=') {
        pos_++;
        if (peek() == '=') {
          pos_++;
          result = (result == parse_expression()) ? 1.0f : 0.0f;
        }
      }
      else if (op == '!') {
        pos_++;
        if (peek() == '=') {
          pos_++;
          result = (result != parse_expression()) ? 1.0f : 0.0f;
        }
      }
      else {
        break;
      }
    }

    return result;
  }

  float parse_logical_and()
  {
    float result = parse_comparison();

    while (true) {
      skip_whitespace();
      size_t start_pos = pos_;
      bool is_and = false;

      if (peek() == '&' && expr_[pos_ + 1] == '&') {
        pos_ += 2; /* Consume && */
        is_and = true;
      }
      else if (peek() == '&') {
        pos_++; /* Consume single & */
        is_and = true;
      }
      else if (match_string("and")) {
        is_and = true;
      }

      if (is_and) {
        float right = parse_comparison();
        result = (result != 0.0f && right != 0.0f) ? 1.0f : 0.0f;
      }
      else {
        pos_ = start_pos;
        break;
      }
    }

    return result;
  }

  float parse_logical_or()
  {
    float result = parse_logical_and();

    while (true) {
      skip_whitespace();
      size_t start_pos = pos_;
      bool is_or = false;

      if (peek() == '|' && expr_[pos_ + 1] == '|') {
        pos_ += 2; /* Consume || */
        is_or = true;
      }
      else if (peek() == '|') {
        pos_++; /* Consume single | */
        is_or = true;
      }
      else if (match_string("or")) {
        is_or = true;
      }

      if (is_or) {
        float right = parse_logical_and();
        result = (result != 0.0f || right != 0.0f) ? 1.0f : 0.0f;
      }
      else {
        pos_ = start_pos;
        break;
      }
    }

    return result;
  }

  float parse_ternary()
  {
    float result = parse_logical_or();

    skip_whitespace();
    if (peek() == '?') {
      pos_++;
      float true_value = parse_ternary();
      skip_whitespace();
      if (peek() == ':') {
        pos_++;
      }
      float false_value = parse_ternary();
      return result != 0.0f ? true_value : false_value;
    }

    return result;
  }

 public:
  ExpressionEvaluator(const char *expr, const float *vars, int num_vars)
      : expr_(expr), pos_(0), vars_(vars), num_vars_(num_vars)
  {
  }

  float evaluate()
  {
    return parse_ternary();
  }
};

/* Helper class to convert expression to GPU node graph */
class GPUExpressionBuilder {
 private:
  GPUMaterial *mat_;
  const char *expr_;
  size_t pos_;
  GPUNodeLink *input_a_;
  GPUNodeLink *input_b_;
  GPUNodeLink *input_c_;

  char peek() const
  {
    return expr_[pos_];
  }

  char get()
  {
    return expr_[pos_++];
  }

  void skip_whitespace()
  {
    while (peek() == ' ' || peek() == '\t') {
      pos_++;
    }
  }

  GPUNodeLink *parse_number()
  {
    skip_whitespace();
    float result = 0.0f;
    float decimal = 0.0f;
    int decimal_places = 0;
    bool negative = false;

    if (peek() == '-') {
      negative = true;
      pos_++;
    }

    while (peek() >= '0' && peek() <= '9') {
      result = result * 10.0f + (get() - '0');
    }

    if (peek() == '.') {
      pos_++;
      while (peek() >= '0' && peek() <= '9') {
        decimal = decimal * 10.0f + (get() - '0');
        decimal_places++;
      }
    }

    result += decimal / powf(10.0f, float(decimal_places));
    float value = negative ? -result : result;
    return GPU_constant(&value);
  }

  bool match_string(const char *str)
  {
    size_t i = 0;
    while (str[i] != '\0') {
      if (expr_[pos_ + i] != str[i]) {
        return false;
      }
      i++;
    }
    char next = expr_[pos_ + i];
    if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
        (next >= '0' && next <= '9') || next == '.')
    {
      return false;
    }
    pos_ += i;
    return true;
  }

  GPUNodeLink *parse_variable()
  {
    skip_whitespace();
    size_t start_pos = pos_;

    if (match_string("pi")) {
      float pi_value = float(M_PI);
      return GPU_constant(&pi_value);
    }
    pos_ = start_pos;

    if (peek() == '\xCF' && expr_[pos_ + 1] == '\x80') {
      pos_ += 2;
      float pi_value = float(M_PI);
      return GPU_constant(&pi_value);
    }

    char c = get();
    switch (c) {
      case 'a':
        return input_a_;
      case 'b':
        return input_b_;
      case 'c':
        return input_c_;
      case 'e': {
        float e_value = float(M_E);
        return GPU_constant(&e_value);
      }
      default: {
        float zero = 0.0f;
        return GPU_constant(&zero);
      }
    }
  }

  GPUNodeLink *parse_function();
  GPUNodeLink *parse_factor();
  GPUNodeLink *parse_power();
  GPUNodeLink *parse_term();
  GPUNodeLink *parse_expression();
  GPUNodeLink *parse_comparison();
  GPUNodeLink *parse_logical_and();
  GPUNodeLink *parse_logical_or();
  GPUNodeLink *parse_ternary();

  bool is_factor_start()
  {
    skip_whitespace();
    char c = peek();
    return (c >= '0' && c <= '9') || c == '(' || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '\xCF';
  }

 public:
  GPUExpressionBuilder(
      GPUMaterial *mat, const char *expr, GPUNodeLink *a, GPUNodeLink *b, GPUNodeLink *c)
      : mat_(mat), expr_(expr), pos_(0), input_a_(a), input_b_(b), input_c_(c)
  {
  }

  GPUNodeLink *build()
  {
    try {
      return parse_ternary();
    }
    catch (...) {
      float zero = 0.0f;
      return GPU_constant(&zero);
    }
  }
};

GPUNodeLink *GPUExpressionBuilder::parse_factor()
{
  skip_whitespace();
  char c = peek();

  if (c == '(') {
    pos_++;
    GPUNodeLink *result = parse_ternary();
    skip_whitespace();
    if (peek() == ')') {
      pos_++;
    }
    return result;
  }

  if ((c >= '0' && c <= '9') || c == '-' || c == '.') {
    return parse_number();
  }

  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
    return parse_function();
  }

  float zero = 0.0f;
  return GPU_constant(&zero);
}

GPUNodeLink *GPUExpressionBuilder::parse_function()
{
  skip_whitespace();
  size_t start_pos = pos_;
  float zero = 0.0f;
  float one = 1.0f;

  /* Try parsing function calls with parentheses */
  if (match_string("min")) {
    skip_whitespace();
    if (peek() == '(') {
      pos_++;
      GPUNodeLink *a = parse_ternary();
      skip_whitespace();
      if (peek() == ',') {
        pos_++;
      }
      GPUNodeLink *b = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      GPUNodeLink *result;
      GPU_link(mat_, "math_minimum", a, b, GPU_constant(&zero), &result);
      return result;
    }
  }
  pos_ = start_pos;

  if (match_string("max")) {
    skip_whitespace();
    if (peek() == '(') {
      pos_++;
      GPUNodeLink *a = parse_ternary();
      skip_whitespace();
      if (peek() == ',') {
        pos_++;
      }
      GPUNodeLink *b = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      GPUNodeLink *result;
      GPU_link(mat_, "math_maximum", a, b, GPU_constant(&zero), &result);
      return result;
    }
  }
  pos_ = start_pos;

  if (match_string("pow")) {
    skip_whitespace();
    if (peek() == '(') {
      pos_++;
      GPUNodeLink *base = parse_ternary();
      skip_whitespace();
      if (peek() == ',') {
        pos_++;
      }
      GPUNodeLink *exp = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      GPUNodeLink *result;
      GPU_link(mat_, "math_power", base, exp, GPU_constant(&zero), &result);
      return result;
    }
  }
  pos_ = start_pos;

  if (match_string("sqrt")) {
    skip_whitespace();
    if (peek() == '(') {
      pos_++;
      GPUNodeLink *val = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      GPUNodeLink *result;
      GPU_link(mat_, "math_sqrt", val, GPU_constant(&zero), GPU_constant(&zero), &result);
      return result;
    }
  }
  pos_ = start_pos;

  if (match_string("abs")) {
    skip_whitespace();
    if (peek() == '(') {
      pos_++;
      GPUNodeLink *val = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      GPUNodeLink *result;
      GPU_link(mat_, "math_absolute", val, GPU_constant(&zero), GPU_constant(&zero), &result);
      return result;
    }
  }
  pos_ = start_pos;

  if (match_string("sin")) {
    skip_whitespace();
    if (peek() == '(') {
      pos_++;
      GPUNodeLink *val = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      GPUNodeLink *result;
      GPU_link(mat_, "math_sine", val, GPU_constant(&zero), GPU_constant(&zero), &result);
      return result;
    }
  }
  pos_ = start_pos;

  if (match_string("cos")) {
    skip_whitespace();
    if (peek() == '(') {
      pos_++;
      GPUNodeLink *val = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      GPUNodeLink *result;
      GPU_link(mat_, "math_cosine", val, GPU_constant(&zero), GPU_constant(&zero), &result);
      return result;
    }
  }
  pos_ = start_pos;

  if (match_string("tan")) {
    skip_whitespace();
    if (peek() == '(') {
      pos_++;
      GPUNodeLink *val = parse_ternary();
      skip_whitespace();
      if (peek() == ')') {
        pos_++;
      }
      GPUNodeLink *result;
      GPU_link(mat_, "math_tangent", val, GPU_constant(&zero), GPU_constant(&zero), &result);
      return result;
    }
  }
  pos_ = start_pos;

  /* If no function matched, try variable */
  return parse_variable();
}

GPUNodeLink *GPUExpressionBuilder::parse_power()
{
  GPUNodeLink *result = parse_factor();
  float zero = 0.0f;

  while (true) {
    skip_whitespace();
    if (peek() == '^') {
      pos_++;
      GPUNodeLink *exponent = parse_factor();
      GPUNodeLink *new_result;
      GPU_link(mat_, "math_power", result, exponent, GPU_constant(&zero), &new_result);
      result = new_result;
    }
    else if (is_factor_start()) {
      GPUNodeLink *factor = parse_factor();
      GPUNodeLink *new_result;
      GPU_link(mat_, "math_multiply", result, factor, GPU_constant(&zero), &new_result);
      result = new_result;
    }
    else {
      break;
    }
  }

  return result;
}

GPUNodeLink *GPUExpressionBuilder::parse_term()
{
  GPUNodeLink *result = parse_power();
  float zero = 0.0f;

  while (true) {
    skip_whitespace();
    char op = peek();

    if (op == '*') {
      pos_++;
      GPUNodeLink *right = parse_power();
      GPUNodeLink *new_result;
      GPU_link(mat_, "math_multiply", result, right, GPU_constant(&zero), &new_result);
      result = new_result;
    }
    else if (op == '/') {
      pos_++;
      GPUNodeLink *divisor = parse_power();
      GPUNodeLink *new_result;
      GPU_link(mat_, "math_divide", result, divisor, GPU_constant(&zero), &new_result);
      result = new_result;
    }
    else if (op == '%') {
      pos_++;
      GPUNodeLink *divisor = parse_power();
      GPUNodeLink *new_result;
      GPU_link(mat_, "math_modulo", result, divisor, GPU_constant(&zero), &new_result);
      result = new_result;
    }
    else {
      break;
    }
  }

  return result;
}

GPUNodeLink *GPUExpressionBuilder::parse_expression()
{
  GPUNodeLink *result = parse_term();
  float zero = 0.0f;

  while (true) {
    skip_whitespace();
    char op = peek();

    if (op == '+') {
      pos_++;
      GPUNodeLink *right = parse_term();
      GPUNodeLink *new_result;
      GPU_link(mat_, "math_add", result, right, GPU_constant(&zero), &new_result);
      result = new_result;
    }
    else if (op == '-') {
      pos_++;
      GPUNodeLink *right = parse_term();
      GPUNodeLink *new_result;
      GPU_link(mat_, "math_subtract", result, right, GPU_constant(&zero), &new_result);
      result = new_result;
    }
    else {
      break;
    }
  }

  return result;
}

GPUNodeLink *GPUExpressionBuilder::parse_comparison()
{
  GPUNodeLink *result = parse_expression();
  float zero = 0.0f;
  float one = 1.0f;

  while (true) {
    skip_whitespace();
    char op = peek();

    if (op == '<') {
      pos_++;
      if (peek() == '=') {
        pos_++;
        /* a <= b is equivalent to !(a > b), or (a > b) ? 0 : 1 */
        GPUNodeLink *right = parse_expression();
        GPUNodeLink *gt_result;
        GPU_link(mat_, "math_greater_than", result, right, GPU_constant(&zero), &gt_result);
        /* Invert: 1 - gt_result */
        GPUNodeLink *new_result;
        GPU_link(mat_,
                 "math_subtract",
                 GPU_constant(&one),
                 gt_result,
                 GPU_constant(&zero),
                 &new_result);
        result = new_result;
      }
      else {
        GPUNodeLink *right = parse_expression();
        GPUNodeLink *new_result;
        GPU_link(mat_, "math_less_than", result, right, GPU_constant(&zero), &new_result);
        result = new_result;
      }
    }
    else if (op == '>') {
      pos_++;
      if (peek() == '=') {
        pos_++;
        /* a >= b is equivalent to !(a < b), or (a < b) ? 0 : 1 */
        GPUNodeLink *right = parse_expression();
        GPUNodeLink *lt_result;
        GPU_link(mat_, "math_less_than", result, right, GPU_constant(&zero), &lt_result);
        /* Invert: 1 - lt_result */
        GPUNodeLink *new_result;
        GPU_link(mat_,
                 "math_subtract",
                 GPU_constant(&one),
                 lt_result,
                 GPU_constant(&zero),
                 &new_result);
        result = new_result;
      }
      else {
        GPUNodeLink *right = parse_expression();
        GPUNodeLink *new_result;
        GPU_link(mat_, "math_greater_than", result, right, GPU_constant(&zero), &new_result);
        result = new_result;
      }
    }
    else if (op == '=') {
      pos_++;
      if (peek() == '=') {
        pos_++;
        /* a == b is equivalent to compare(a, b, epsilon) where epsilon is very small */
        GPUNodeLink *right = parse_expression();
        float epsilon = 1e-5f;
        GPUNodeLink *new_result;
        GPU_link(mat_, "math_compare", result, right, GPU_constant(&epsilon), &new_result);
        result = new_result;
      }
    }
    else if (op == '!') {
      pos_++;
      if (peek() == '=') {
        pos_++;
        /* a != b is equivalent to !(a == b), or 1 - compare(a, b, epsilon) */
        GPUNodeLink *right = parse_expression();
        float epsilon = 1e-5f;
        GPUNodeLink *eq_result;
        GPU_link(mat_, "math_compare", result, right, GPU_constant(&epsilon), &eq_result);
        /* Invert: 1 - eq_result */
        GPUNodeLink *new_result;
        GPU_link(mat_,
                 "math_subtract",
                 GPU_constant(&one),
                 eq_result,
                 GPU_constant(&zero),
                 &new_result);
        result = new_result;
      }
    }
    else {
      break;
    }
  }

  return result;
}

GPUNodeLink *GPUExpressionBuilder::parse_logical_and()
{
  GPUNodeLink *result = parse_comparison();
  float zero = 0.0f;

  while (true) {
    skip_whitespace();
    size_t start_pos = pos_;
    bool is_and = false;

    if (peek() == '&' && expr_[pos_ + 1] == '&') {
      pos_ += 2; /* Consume && */
      is_and = true;
    }
    else if (peek() == '&') {
      pos_++; /* Consume single & */
      is_and = true;
    }
    else if (match_string("and")) {
      is_and = true;
    }

    if (is_and) {
      GPUNodeLink *right = parse_comparison();
      /* Logical AND: both must be non-zero (true)
       * In boolean logic: AND = min(a, b) when a, b are 0 or 1
       * We use: (a != 0) AND (b != 0) = min(a != 0, b != 0)
       */
      GPUNodeLink *new_result;
      GPU_link(mat_, "math_minimum", result, right, GPU_constant(&zero), &new_result);
      result = new_result;
    }
    else {
      pos_ = start_pos;
      break;
    }
  }

  return result;
}

GPUNodeLink *GPUExpressionBuilder::parse_logical_or()
{
  GPUNodeLink *result = parse_logical_and();
  float zero = 0.0f;

  while (true) {
    skip_whitespace();
    size_t start_pos = pos_;
    bool is_or = false;

    if (peek() == '|' && expr_[pos_ + 1] == '|') {
      pos_ += 2; /* Consume || */
      is_or = true;
    }
    else if (peek() == '|') {
      pos_++; /* Consume single | */
      is_or = true;
    }
    else if (match_string("or")) {
      is_or = true;
    }

    if (is_or) {
      GPUNodeLink *right = parse_logical_and();
      /* Logical OR: at least one must be non-zero (true)
       * In boolean logic: OR = max(a, b) when a, b are 0 or 1
       * We use: (a != 0) OR (b != 0) = max(a != 0, b != 0)
       */
      GPUNodeLink *new_result;
      GPU_link(mat_, "math_maximum", result, right, GPU_constant(&zero), &new_result);
      result = new_result;
    }
    else {
      pos_ = start_pos;
      break;
    }
  }

  return result;
}

GPUNodeLink *GPUExpressionBuilder::parse_ternary()
{
  GPUNodeLink *result = parse_logical_or();
  float zero = 0.0f;
  float one = 1.0f;

  skip_whitespace();
  if (peek() == '?') {
    pos_++;
    GPUNodeLink *true_value = parse_ternary();
    skip_whitespace();
    if (peek() == ':') {
      pos_++;
    }
    GPUNodeLink *false_value = parse_ternary();

    /* Ternary: condition ? true_val : false_val
     * Implementation: (condition > 0) * true_val + (condition <= 0) * false_val
     * Using mix: mix(false_val, true_val, condition > 0)
     *
     * Simplified approach:
     * - If condition != 0: clamp it to 1
     * - result = false_val + condition_clamped * (true_val - false_val)
     *
     * Better: Compare condition > 0, then use that as factor for mix
     */

    /* Check if condition is greater than 0 */
    GPUNodeLink *condition_bool;
    GPU_link(mat_,
             "math_greater_than",
             result,
             GPU_constant(&zero),
             GPU_constant(&zero),
             &condition_bool);

    /* Calculate (true_value - false_value) */
    GPUNodeLink *diff;
    GPU_link(mat_, "math_subtract", true_value, false_value, GPU_constant(&zero), &diff);

    /* Calculate condition_bool * diff */
    GPUNodeLink *scaled_diff;
    GPU_link(mat_, "math_multiply", condition_bool, diff, GPU_constant(&zero), &scaled_diff);

    /* Calculate false_value + scaled_diff */
    GPUNodeLink *final_result;
    GPU_link(mat_, "math_add", false_value, scaled_diff, GPU_constant(&zero), &final_result);

    return final_result;
  }

  return result;
}

static int node_gpu_material(GPUMaterial *material,
                             bNode *node,
                             bNodeExecData * /*execdata*/,
                             GPUNodeStack *inputs,
                             GPUNodeStack *outputs)
{
  NodeCMPExpression *storage = (NodeCMPExpression *)node->storage;
  const char *expr = (storage && storage->expression) ? storage->expression : "a";

  /* Get input links */
  GPUNodeLink *input_a = inputs[0].link ? inputs[0].link : GPU_uniform(inputs[0].vec);
  GPUNodeLink *input_b = inputs[1].link ? inputs[1].link : GPU_uniform(inputs[1].vec);
  GPUNodeLink *input_c = inputs[2].link ? inputs[2].link : GPU_uniform(inputs[2].vec);

  /* Build GPU node graph from expression */
  GPUExpressionBuilder builder(material, expr, input_a, input_b, input_c);
  GPUNodeLink *result = builder.build();

  /* Set output */
  outputs[0].link = result;

  return true;
}

class ExpressionMultiFunction : public blender::fn::multi_function::MultiFunction {
 private:
  std::string expression_;
  blender::fn::multi_function::Signature signature_;

 public:
  ExpressionMultiFunction(const char *expr) : expression_(expr)
  {
    blender::fn::multi_function::SignatureBuilder builder("Expression", signature_);
    builder.single_input<float>("a");
    builder.single_input<float>("b");
    builder.single_input<float>("c");
    builder.single_output<float>("Result");
    this->set_signature(&signature_);
  }

  void call(const blender::IndexMask &mask,
            blender::fn::multi_function::Params params,
            blender::fn::multi_function::Context /*context*/) const override
  {
    const blender::VArray<float> &input_a = params.readonly_single_input<float>(0, "a");
    const blender::VArray<float> &input_b = params.readonly_single_input<float>(1, "b");
    const blender::VArray<float> &input_c = params.readonly_single_input<float>(2, "c");
    blender::MutableSpan<float> output = params.uninitialized_single_output<float>(3, "Result");

    mask.foreach_index([&](const int64_t i) {
      float vars[] = {input_a[i], input_b[i], input_c[i]};
      ExpressionEvaluator eval(expression_.c_str(), vars, 3);
      output[i] = eval.evaluate();
    });
  }
};

static void node_build_multi_function(blender::nodes::NodeMultiFunctionBuilder &builder)
{
  const bNode &node = builder.node();
  NodeCMPExpression *storage = (NodeCMPExpression *)node.storage;

  const char *expr = (storage && storage->expression) ? storage->expression : "a";
  builder.construct_and_set_matching_fn<ExpressionMultiFunction>(expr);
}

}  // namespace blender::nodes::node_composite_expression_cc

static void register_node_type_cmp_expression()
{
  namespace file_ns = blender::nodes::node_composite_expression_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeExpression", CMP_NODE_EXPRESSION);
  ntype.ui_name = "Expression";
  ntype.ui_description = "Evaluate mathematical expression on float inputs";
  ntype.enum_name_legacy = "EXPRESSION";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::cmp_node_expression_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_expression;
  ntype.initfunc = file_ns::node_composit_init_expression;
  ntype.freefunc = file_ns::node_free_expression;
  ntype.copyfunc = file_ns::node_copy_expression;
  ntype.gpu_fn = file_ns::node_gpu_material;
  ntype.build_multi_function = file_ns::node_build_multi_function;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_expression)
