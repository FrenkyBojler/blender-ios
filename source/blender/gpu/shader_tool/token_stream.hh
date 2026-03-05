/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 *
 */

#pragma once

#include "enums.hh"
#include "utils.hh"

#include <array>

namespace blender::gpu::shader::parser {

struct Token;

/**
 * Turns string into token.
 */
struct LexerBase : lexit::TokenBuffer {
  static const std::array<CharClass, 128> bsl_char_class_table;
  static const std::array<CharClass, 128> default_char_class_table;

  /** Compact visualization of token_types.  */
  std::string_view token_types_str() const
  {
    return std::string_view((const char *)types_.get(), size_);
  }

 protected:
  /* Change words into keyword (ex: `if`, `struct`, `template`). */
  void identify_keywords();
};

/**
 * Consider numbers as words (to avoid splitting identifiers).
 * Does not merge newlines and spaces.
 */
struct SimpleLexer : LexerBase {
  void lexical_analysis(std::string_view input)
  {
    process(input, bsl_char_class_table.data());
  }
};

/**
 * Allow recognition of common operators and numbers. Merge white-spaces.
 */
struct ExpressionLexer : LexerBase {
  void lexical_analysis(std::string_view input)
  {
    process(input, default_char_class_table.data());
    merge_complex_literals();
    identify_keywords();
  }
};

/**
 * Allow recognition of operators and numbers. Merge white-spaces.
 * However, doesn't merge angle bracket with other tokens in order to use them for template
 * expressions parsing.
 */
struct FullLexer : LexerBase {
  void lexical_analysis(std::string_view input)
  {
    process(input, bsl_char_class_table.data());
    merge_complex_literals();
    identify_keywords();
  }
};

/**
 * Create semantic scopes from token stream.
 * Also creates mapping table from token to scope to have bi-directional mapping.
 */
struct ParserBase {
  const LexerBase &lex;

  /** Compact visualization of scope_types.  */
  std::string_view scope_types_str;

  /* --- Structure of Array style data for scopes. --- */

  /** Range of token per scope. */
  std::vector<ScopeType> scope_types;
  /** Range of token per scope. */
  std::vector<IndexRange> scope_ranges;
  /** Index of bottom most scope per token. */
  std::vector<int> token_scope;

  ParserBase(const LexerBase &lex) : lex(lex) {}

  /* Return the i'th token. */
  Token operator[](int i) const;

 protected:
  void build_scope_tree(report_callback &report_error);
  void build_token_to_scope_map();

 private:
  void update_string_view();
};

/* Don't do anything. No access to scopes is allowed. */
struct NullParser : ParserBase {
  NullParser(const LexerBase &lex) : ParserBase(lex) {}

  void semantic_analysis(report_callback & /*report_error*/)
  {
    scope_types = {};
    scope_ranges = {};
  }
};

/* Do not parse. Creates a single global scope containing all tokens. */
struct DummyParser : ParserBase {
  DummyParser(const LexerBase &lex) : ParserBase(lex) {}

  void semantic_analysis(report_callback & /*report_error*/)
  {
    scope_types = {ScopeType::Global};
    scope_ranges = {IndexRange(0, lex.size())};
    build_token_to_scope_map();
  }
};

struct FullParser : ParserBase {
  FullParser(const LexerBase &lex) : ParserBase(lex) {}

  void semantic_analysis(report_callback &report_error)
  {
    build_scope_tree(report_error);
    build_token_to_scope_map();
  }
};

}  // namespace blender::gpu::shader::parser
