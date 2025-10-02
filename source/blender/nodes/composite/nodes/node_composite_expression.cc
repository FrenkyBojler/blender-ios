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
        (next >= '0' && next <= '9') || next == '.') {
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

      if (peek() == '&') {
        pos_++;
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

      if (peek() == '|') {
        pos_++;
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

static int node_gpu_material(GPUMaterial *material,
                             bNode *node,
                             bNodeExecData * /*execdata*/,
                             GPUNodeStack *inputs,
                             GPUNodeStack *outputs)
{
  return GPU_stack_link(material, node, "node_composite_map_value_legacy", inputs, outputs);
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
  ntype.nclass = NODE_CLASS_INPUT;
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