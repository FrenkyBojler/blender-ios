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

namespace blender::gpu::shader::parser {

struct Token;

/* Used to select at which stage to stop.
 *  */
enum ParserStage {
  TokenizePreprocessor,
  Tokenize,
  MergeTokens,
  IdentifyKeywords,
  BuildScopeTree,
};

struct LexerData {
  /** Token Data. Backing memory for the spans. */
  std::vector<TokenType> token_types_data;
  std::vector<uint32_t> token_sizes_data;
  std::vector<uint32_t> token_offsets_data;
};

/**
 * Turns string into token.
 */
struct LexerBase {
  /** The lexer's input string. */
  std::string_view str;

  /** Compact visualization of token_types.  */
  std::string_view token_types_str;

  /* --- Structure of Array style data for tokens. --- */

  /** Token type per token. */
  MutableSpan<TokenType> token_types;
  /** Size of the raw token before token merging. */
  MutableSpan<uint32_t> token_sizes;
  /** Ranges of characters per token. */
  OffsetIndices token_offsets;

  /* Note: This binds the data to this lexer until it is freed. */
  LexerBase(std::string_view input, LexerData &data);

 protected:
  /* Create tokens based on character stream. */
  void tokenize(bool only_preprocessor_tokens);
  /* Merge tokens (ex: '2','.','e','-','3` into '2.e-3`). */
  void merge_tokens();
  /* Change words into keyword (ex: `if`, `struct`, `template`). */
  void identify_keywords();

 private:
  void update_string_view();
};

/**
 * Consider numbers as words (to avoid splitting identifiers).
 * Does not merge newlines and spaces.
 */
struct PreprocessorLexer : LexerBase {
  PreprocessorLexer(std::string_view input, LexerData &data) : LexerBase(input, data)
  {
    tokenize(true);
  }
};

/**
 * Allow recognition of common operators and numbers. Merge whitespaces.
 */
struct ExpressionLexer : LexerBase {
  ExpressionLexer(std::string_view input, LexerData &data) : LexerBase(input, data)
  {
    tokenize(false);
    merge_tokens();
  }
};

struct FullLexer : LexerBase {
  FullLexer(std::string_view input, LexerData &data) : LexerBase(input, data)
  {
    tokenize(false);
    merge_tokens();
    identify_keywords();
  }
};

struct ParserData {
  /** Range of token per scope. */
  std::vector<ScopeType> scope_types;
  /** Range of token per scope. */
  std::vector<IndexRange> scope_ranges;
  /** Index of bottom most scope per token. */
  std::vector<int> token_scope;
};

/**
 * Create semantic scopes from token stream.
 * Also creates mapping table from token to scope to have bi-directional mapping.
 */
struct ParserBase {
  const LexerBase *lex;

  /** Compact visualization of scope_types.  */
  std::string_view scope_types_str;

  /* --- Structure of Array style data for scopes. --- */

  /** Range of token per scope. */
  std::vector<ScopeType> *scope_types;
  /** Range of token per scope. */
  std::vector<IndexRange> *scope_ranges;
  /** Index of bottom most scope per token. */
  std::vector<int> *token_scope;

  ParserBase(const LexerBase &lex, ParserData &data)
      : lex(&lex),
        scope_types(&data.scope_types),
        scope_ranges(&data.scope_ranges),
        token_scope(&data.token_scope)
  {
  }

  /* Return the i'th token. */
  Token operator[](int i) const;

 protected:
  void build_scope_tree(report_callback &report_error);
  void build_token_to_scope_map();

 private:
  void update_string_view();
};

/* Do not parse. Creates a single global scope containing all tokens. */
struct DummyParser : ParserBase {
  DummyParser(LexerBase &lex, ParserData &data) : ParserBase(lex, data)
  {
    *scope_types = {ScopeType::Global};
    *scope_ranges = {IndexRange(0, lex.token_types.size())};
    build_token_to_scope_map();
  }
};

struct FullParser : ParserBase {
  FullParser(LexerBase &lex, ParserData &data, report_callback &report_error)
      : ParserBase(lex, data)
  {
    build_scope_tree(report_error);
    build_token_to_scope_map();
  }
};

struct LexerParserData {
  LexerData lexer_data;
  ParserData parser_data;
};

}  // namespace blender::gpu::shader::parser
