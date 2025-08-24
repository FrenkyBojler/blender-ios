/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"
#include <variant>

namespace blender::dna::lex {
struct Token {
  StringRef where;
};

struct BreakLineToken : public Token {};

struct IdentifierToken : public Token {};

struct StringLiteralToken : public Token {};

struct IntLiteralToken : public Token {
  int64_t value;
};

struct SymbolToken : public Token {};

struct KeywordToken : public Token {};

using TokenVariant = std::variant<BreakLineToken,
                                  IdentifierToken,
                                  IntLiteralToken,
                                  SymbolToken,
                                  KeywordToken,
                                  StringLiteralToken>;

struct TokenIterator {
  /** Last token that fails to match a token request. */
  TokenVariant *last_unmatched = nullptr;

 private:
  /** Token stream. */
  Vector<TokenVariant> token_stream_;
  /** Return points to use for roll back when parser fails to parse tokens. */
  Vector<TokenVariant *> waypoints_;
  /** Pointer to next token to iterate. */
  TokenVariant *next_ = nullptr;

  /** Print the line where an unkown token was found. */
  void print_unkown_token(StringRef filepath, StringRef text, const char *where);

  void skip_break_lines();

 public:
  /** Iterates over the input text looking for tokens. */
  void process_text(StringRef filepath, StringRef text);

  /**
   * Add the current token as waypoint, in case the token parser needs the iterator to roll
   * back.
   */
  void push_waypoint();

  /**
   * Removes the last waypoint, if `success==false` the iterator rolls back to this last
   * waypoint.
   */
  void end_waypoint(bool success);

  /** Return the pointer to the next token, and advances the iterator. */
  TokenVariant *next_variant();

  /** Checks if the token iterator has reach the last token. */
  bool has_finish();

  /**
   * Return the next token if it type matches to `Type`.
   * Break lines are skipped for non break line requested tokens.
   */
  template<class Type> Type *next()
  {
    TokenVariant *current_next = next_;
    if constexpr (!std::is_same_v<Type, BreakLineToken>) {
      skip_break_lines();
    }
    if (next_ < token_stream_.end() && std::holds_alternative<Type>(*next_)) {
      return &std::get<Type>(*next_++);
    }
    if (last_unmatched < next_) {
      last_unmatched = next_;
    }
    next_ = current_next;
    return nullptr;
  }

 private:
  /** Match any whitespace except break lines. */
  void eval_space(const char *&itr, const char *end);
  /** Match break lines. */
  void eval_break_line(const char *&itr, const char *end);
  /** Match identifiers and `C++` keywords. */
  void eval_identifier(const char *&itr, const char *end);
  /** Match single-line comment. */
  void eval_line_comment(const char *&itr, const char *end);
  /** Match a int literal. */
  void eval_int_literal(const char *&itr, const char *end);
  /** Match a multi-line comment. */
  void eval_multiline_comment(const char *&itr, const char *end);
  /** Match a symbol. */
  void eval_symbol(const char *&itr, const char *end);
  /** Match a string or char literal. */
  void eval_string_literal(const char *&itr, const char *end);

  /** Appends a token. */
  template<class TokenType> void append(TokenType &&token)
  {
    token_stream_.append(std::move(token));
  }
};

}  // namespace blender::dna::lex
