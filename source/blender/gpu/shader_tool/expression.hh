/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 *
 * Simple integer logic expression using a Pratt-parser.
 */

#pragma once

#include "token.hh"

namespace blender::gpu::shader::parser {

/**
 * Simple expression parsing and evaluation.
 * Will evaluate starting the given token until the end of the token stream.
 */
class ExpressionParser {
 private:
  Token tok;

 public:
  explicit ExpressionParser(Token start) : tok(start) {}

  int64_t eval()
  {
    int64_t v = expr(0);
    if (peek() != Invalid) {
      throw std::runtime_error("Trailing input");
    }
    return v;
  }

 private:
  int64_t expr(int right_binding_power)
  {
    /* Parse unary operator, evaluate parenthesis, evaluate constant. */
    int64_t left = nud(consume());
    /* While left binding power is greater than the right, continue consuming binary operations. */
    while (binding_power(peek().type()) > right_binding_power) {
      left = led(left, consume());
    }
    return left;
  }

  /* How a token evaluates without left context (e.g. unary operator).
   * Also known as Null-Denotation or NUD. */
  int64_t nud(const Token &t)
  {
    switch (t.type()) {
      case Number:
        return std::stol(t.str());
      case Plus:
        return +expr(unary_binding_power);
      case Minus:
        return -expr(unary_binding_power);
      case Not:
        return !expr(unary_binding_power);
      case BitwiseNot:
        return ~expr(unary_binding_power);
      case ParOpen: {
        /* Parse the whole parenthesis expression. */
        int64_t v = expr(binding_power(ParOpen));
        /* Consume the closing parenthesis. */
        if (consume() != ParClose) {
          throw std::runtime_error("Expected ')'");
        }
        return v;
      }
      default:
        throw std::runtime_error("Invalid expression");
    }
  }

  /* How a token evaluates from left-to-right, on two operands.
   * Also known as Left-Denotation or LED. */
  int64_t led(int64_t left, const Token &t)
  {
    switch (t.type()) {
      case Multiply:
        return left * expr(binding_power(Multiply));
      case Divide:
        return left / expr(binding_power(Divide));
      case Modulo:
        return left % expr(binding_power(Modulo));
      case Plus:
        return left + expr(binding_power(Plus));
      case Minus:
        return left - expr(binding_power(Minus));
#if 0 /* Not implemented yet. */
      case LShift:
        return left << expression(binding_power(LShift));
      case RShift:
        return left >> expression(binding_power(RShift));
#endif
      case LThan:
        return left < expr(binding_power(LThan));
      case LEqual:
        return left <= expr(binding_power(LEqual));
      case GThan:
        return left > expr(binding_power(GThan));
      case GEqual:
        return left >= expr(binding_power(GEqual));
      case Equal:
        return left == expr(binding_power(Equal));
      case NotEqual:
        return left != expr(binding_power(NotEqual));
      case And:
        return left & expr(binding_power(And));
      case Xor:
        return left ^ expr(binding_power(Xor));
      case Or:
        return left | expr(binding_power(Or));
      case LogicalAnd:
        return left && expr(binding_power(LogicalAnd));
      case LogicalOr:
        return left || expr(binding_power(LogicalOr));
      case Question: {
        int64_t tval = expr(binding_power(Question));
        if (consume().type() != Colon) {
          throw std::runtime_error("Expected ':'");
        }
        int64_t fval = expr(binding_power(Colon));
        return left ? tval : fval;
      }
      default:
        throw std::runtime_error("Invalid operator");
    }
  }

  /* Unary operators must have the highest precedence. */
  static constexpr int unary_binding_power = 1000;

  int binding_power(TokenType k)
  {
    switch (k) {
      case Not:
      case BitwiseNot:
        return unary_binding_power;
      case Multiply:
      case Divide:
      case Modulo:
        return 110;
      case Plus:
      case Minus:
        return 100;
#if 0 /* Not implemented yet. */
      case LShift:
      case RShift:
        return 90;
#endif
      case LThan:
      case LEqual:
      case GThan:
      case GEqual:
        return 80;
      case Equal:
      case NotEqual:
        return 70;
      case And:
        return 60;
      case Xor:
        return 50;
      case Or:
        return 40;
      case LogicalAnd:
        return 30;
      case LogicalOr:
        return 20;
      case Question:
        return 10;
      case Colon:
      case ParOpen:
        return 0;
      case Invalid: /* EndOfFile */
        return -1;
      default:
        break;
    }
    throw std::runtime_error("Invalid token");
    return 0;
  }

  Token peek() const
  {
    return tok;
  }

  Token consume()
  {
    Token t = tok;
    tok = tok.next();
    return t;
  }
};

}  // namespace blender::gpu::shader::parser
