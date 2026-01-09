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

/* Used to select at which stage to stop.
 *  */
enum ParserStage {
  TokenizePreprocessor,
  Tokenize,
  MergeTokens,
  IdentifyKeywords,
  BuildScopeTree,
};

/**
 * This a tiny bit more than a token stream as it contains ranges or tokens (called scopes) from
 * syntax. The scopes and tokens have bi-directional mapping.
 */
struct TokenStream {
  /** The lexer's input string. */
  std::string str;
  /** Compact visualization of token_types.  */
  std::string_view token_types_str;
  /** Compact visualization of scope_types.  */
  std::string_view scope_types_str;

  /* Structure of Array style data for tokens. */
  /** Token type per token. */
  MutableSpan<TokenType> token_types;
  /** Size of the raw token before token merging. */
  MutableSpan<uint32_t> token_sizes;
  /** Ranges of characters per token. */
  OffsetIndices token_offsets;

  /* Structure of Array style data for scopes. */
  /** Range of token per scope. */
  std::vector<ScopeType> scope_types;
  /** Range of token per scope. */
  std::vector<IndexRange> scope_ranges;
  /** Index of bottom most scope per token. */
  std::vector<int> token_scope;

  /** Token Data. Backing memory for the span above. TODO(fclem): Move it out of here. */
  std::vector<TokenType> token_types_data;
  std::vector<uint32_t> token_sizes_data;
  std::vector<uint32_t> token_offsets_data;

  void lexical_analysis(ParserStage stop_after);

  void semantic_analysis(ParserStage stop_after, report_callback &report_error);

 private:
  /* Create tokens based on character stream. */
  void tokenize(bool only_preprocessor_tokens);
  /* Merge tokens (ex: '2','.','e','-','3` into '2.e-3`). */
  void merge_tokens();

  void identify_keywords();

  void build_scope_tree(report_callback &report_error);

  void build_token_to_scope_map();
};

}  // namespace blender::gpu::shader::parser
