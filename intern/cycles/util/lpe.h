/* SPDX-FileCopyrightText: 2011-2024 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/string.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

/* Light Path Expression Parser
 *
 * Parser for OSL-style Light Path Expressions used in AOV rendering.
 * Supports regex-like syntax for matching light paths:
 * - C: Camera, R: Reflection, T: Transmission, V: Volume
 * - L: Light, O: Emission, B: Background
 * - D: Diffuse, G: Glossy, S: Singular (combined with event type: RD, RG, etc.)
 * - .: Match any single event
 * - *: Match zero or more events
 * - +: Match one or more events
 * - []: Character set (e.g., [RG] matches R or G)
 */

/* LPE Token types for compiled expression */
enum LPETokenType {
  LPE_TOKEN_EVENT,     /* Specific event (C, RD, L, etc.) */
  LPE_TOKEN_ANY,       /* . - match any single event */
  LPE_TOKEN_STAR,      /* * - match zero or more */
  LPE_TOKEN_PLUS,      /* + - match one or more */
  LPE_TOKEN_OPTIONAL,  /* ? - match zero or one */
  LPE_TOKEN_SET_BEGIN, /* [ - start of character set */
  LPE_TOKEN_SET_END,   /* ] - end of character set */
  LPE_TOKEN_END        /* End of expression */
};

/* LPE operators for combining expressions */
enum LPEOperator {
  LPE_OP_NONE,     /* No operator */
  LPE_OP_OR,       /* | - Union of two patterns */
  LPE_OP_SUBTRACT, /* - - Set difference (A - B) */
  LPE_OP_NEGATE    /* ! - Negation of entire expression */
};

/* LPE tag types for filtering by light group, object, or material */
enum LPETagType { LPE_TAG_NONE, LPE_TAG_LIGHT_GROUP, LPE_TAG_OBJECT, LPE_TAG_MATERIAL };

/* LPE tag for filtering events (e.g., <lgroup:key>, <obj:Floor>) */
struct LPETag {
  LPETagType type;
  string name;
  bool is_negated;  /* For <^name> */
  bool is_wildcard; /* For <*> */

  LPETag() : type(LPE_TAG_NONE), is_negated(false), is_wildcard(false) {}
};

struct LPEToken {
  LPETokenType type;
  int event_mask;      /* Bitmask of allowed events for sets/specific events */
  vector<char> events; /* For character sets */
  bool is_negated;     /* For negated character sets [^...] */
  LPETag tag;          /* Optional tag filter */
};

/* Single pattern (e.g., CDL or C.*L) */
struct LPEPattern {
  vector<LPEToken> tokens;
  string pattern_str; /* Original pattern string for this sub-pattern */
};

class LPEParser {
 public:
  LPEParser();
  ~LPEParser();

  /* Compile an LPE expression string */
  bool compile(const string &expression);

  /* Check if the compiled expression is valid */
  bool is_valid() const
  {
    return is_valid_;
  }

  /* Get the compiled expression */
  const string &get_expression() const
  {
    return expression_;
  }

  /* Get compiled tokens */
  const vector<LPEToken> &get_tokens() const
  {
    return tokens_;
  }

  /* Match a path string against the compiled expression */
  bool match(const string &path) const;

 private:
  string expression_;
  bool is_valid_;
  vector<LPEToken> tokens_;       /* For backward compatibility - single pattern */
  vector<LPEPattern> patterns_;   /* Multiple patterns for OR/SUBTRACT operations */
  vector<LPEOperator> operators_; /* Operators between patterns */
  bool has_negation_;             /* Leading ! operator */

  /* Parse helpers */
  bool parse_tokens();
  bool parse_pattern(const string &pattern_str, LPEPattern &pattern);
  int char_to_event(char c) const;

  /* Matching helpers */
  bool match_recursive(const char *path, size_t token_idx) const;
  bool match_recursive(const char *path, size_t token_idx, const vector<LPEToken> &tokens) const;
  bool match_pattern(const char *path, const LPEPattern &pattern) const;
  bool char_matches_token(char c, const LPEToken &token) const;
};

CCL_NAMESPACE_END
