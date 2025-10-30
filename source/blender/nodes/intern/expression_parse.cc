/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>
#include <iostream>

#include "NOD_expression_parse.hh"

#include "BLT_translation.hh"

#include "expression_parse.hh"

namespace blender::nodes::expression {

class Tokenizer {
 private:
  const StringRef full_str_;
  int64_t i_ = 0;
  const int64_t full_size_;
  std::optional<std::string> error_;
  Vector<Token> tokens_;

 public:
  Tokenizer(const StringRef full_str) : full_str_(full_str), full_size_(full_str.size())
  {
    /* This generally overallocates a bit, but avoids reallocations later. */
    tokens_.reserve(full_str.size());
  }

  TokenizeResult tokenize()
  {
    while (i_ < full_size_) {
      if (error_.has_value()) {
        break;
      }
      const char c = full_str_[i_];
      if (this->is_whitespace(c)) {
        i_++;
        continue;
      }
      if (this->is_digit(c)) {
        this->tokenize_number();
        continue;
      }
      if (this->is_identifier_start(c)) {
        this->tokenize_identifier();
        continue;
      }
      if (this->is_string_start(c)) {
        this->tokenize_string();
        continue;
      }
      if (this->tokenize_special()) {
        continue;
      }
      this->set_invalid_char_error(c);
    }

    if (error_.has_value()) {
      return {std::move(*error_)};
    }
    this->assert_token_strings_in_full_str();
    return {std::move(tokens_)};
  }

  void tokenize_number()
  {
    const int64_t start = i_;
    i_++;
    while (i_ < full_size_) {
      const char c = full_str_[i_];
      if (!this->is_number_continue(c)) {
        break;
      }
      i_++;
    }
    const StringRef number = full_str_.substr(start, i_ - start);
    tokens_.append({TokenType::Number, number});
  }

  void tokenize_identifier()
  {
    const int64_t start = i_;
    i_++;
    while (i_ < full_size_) {
      const char c = full_str_[i_];
      if (!this->is_identifier_continue(c)) {
        break;
      }
      i_++;
    }
    const StringRef identifier = full_str_.substr(start, i_ - start);
    tokens_.append({TokenType::Identifier, identifier});
  }

  [[nodiscard]] bool tokenize_special()
  {
    const char first = full_str_[i_];
    const std::optional<char> second = i_ + 1 < full_size_ ?
                                           std::make_optional(full_str_[i_ + 1]) :
                                           std::nullopt;

    auto add_special = [&](const int length) {
      tokens_.append({TokenType::Special, StringRef(full_str_.data() + i_, length)});
      i_ += length;
      return true;
    };

    switch (first) {
      case '+':
      case '-':
      case '*':
      case '/':
      case '(':
      case ')':
      case ',':
      case ':':
      case '?': {
        return add_special(1);
      }
      case '<':
      case '>': {
        if (second == '=') {
          return add_special(2);
        }
        if (second == first) {
          return add_special(2);
        }
        return add_special(1);
      }
      case '=': {
        if (second == '=') {
          return add_special(2);
        }
        return add_special(1);
      }
      default: {
        return false;
      }
    }
  }

  void tokenize_string()
  {
    const int64_t start = i_;
    i_++;
    while (i_ < full_size_) {
      const char c = full_str_[i_];
      if (c == '"') {
        break;
      }
      i_++;
    }
    if (i_ == full_size_) {
      this->set_error(TIP_("Unterminated string"));
      return;
    }
    i_++;
    const StringRef string = full_str_.substr(start, i_ - start);
    tokens_.append({TokenType::String, string});
  }

  void set_invalid_char_error(const char c)
  {
    if (std::isprint(c)) {
      this->set_error(fmt::format("{}: '{}'", TIP_("Invalid character"), c));
    }
    else {
      this->set_error(fmt::format("{}: {:#x}", TIP_("Invalid character"), uint32_t(c)));
    }
  }

  void assert_token_strings_in_full_str()
  {
#ifndef NDEBUG
    for (const Token &token : tokens_) {
      const StringRef str = token.str;
      BLI_assert(full_str_.data() <= str.data());
      BLI_assert(str.data() + str.size() <= full_str_.data() + full_str_.size());
    }
#endif
  }

  void set_error(std::string error)
  {
    error_ = std::move(error);
  }

  bool is_number_start(const char c) const
  {
    return this->is_digit(c);
  }

  bool is_number_continue(const char c) const
  {
    return this->is_number_start(c) || strchr(".eExXoO", c);
  }

  bool is_digit(const char c) const
  {
    return '0' <= c && c <= '9';
  }

  bool is_identifier_start(const char c) const
  {
    return this->is_lowercase_ascii(c) || this->is_uppercase_ascii(c) || c == '_';
  }

  bool is_identifier_continue(const char c) const
  {
    return this->is_identifier_start(c) || this->is_digit(c);
  }

  bool is_lowercase_ascii(const char c) const
  {
    return 'a' <= c && c <= 'z';
  }

  bool is_uppercase_ascii(const char c) const
  {
    return 'A' <= c && c <= 'Z';
  }

  bool is_whitespace(const char c) const
  {
    return ELEM(c, ' ', '\t', '\n', '\r');
  }

  bool is_string_start(const char c) const
  {
    return c == '"';
  }
};

TokenizeResult tokenize(const StringRef expression)
{
  Tokenizer tokenizer(expression);
  return tokenizer.tokenize();
}

ast::Expr *parse(ResourceScope &scope, const StringRef expression, std::ostream &r_errors)
{
  UNUSED_VARS(scope, expression, r_errors);
  r_errors << "Not implemented";
  return nullptr;
}

}  // namespace blender::nodes::expression
