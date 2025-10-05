/* SPDX-FileCopyrightText: 2011-2024 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "util/lpe.h"
#include "util/log.h"

CCL_NAMESPACE_BEGIN

LPEParser::LPEParser() : is_valid_(false) {}

LPEParser::~LPEParser() {}

bool LPEParser::compile(const string &expression)
{
  expression_ = expression;
  tokens_.clear();

  if (expression.empty()) {
    is_valid_ = false;
    return false;
  }

  if (!parse_tokens()) {
    is_valid_ = false;
    return false;
  }

  is_valid_ = true;
  return true;
}

bool LPEParser::parse_tokens()
{
  const char *ptr = expression_.c_str();

  while (*ptr) {
    /* Skip spaces */
    if (*ptr == ' ') {
      ptr++;
      continue;
    }

    LPEToken token;
    token.event_mask = 0;

    /* Check for wildcards */
    if (*ptr == '*') {
      token.type = LPE_TOKEN_STAR;
      tokens_.push_back(token);
      ptr++;
      continue;
    }
    else if (*ptr == '+') {
      token.type = LPE_TOKEN_PLUS;
      tokens_.push_back(token);
      ptr++;
      continue;
    }
    else if (*ptr == '?') {
      token.type = LPE_TOKEN_OPTIONAL;
      tokens_.push_back(token);
      ptr++;
      continue;
    }
    else if (*ptr == '.') {
      token.type = LPE_TOKEN_ANY;
      tokens_.push_back(token);
      ptr++;
      continue;
    }
    /* Character sets [DGS] */
    else if (*ptr == '[') {
      ptr++;
      token.type = LPE_TOKEN_EVENT;

      while (*ptr && *ptr != ']') {
        int event = char_to_event(*ptr);
        if (event >= 0) {
          token.event_mask |= (1 << event);
          token.events.push_back(*ptr);
        }
        ptr++;
      }

      if (*ptr == ']') {
        ptr++;
      }

      if (token.event_mask != 0) {
        tokens_.push_back(token);
      }
      continue;
    }
    /* Single event character */
    else {
      int event = char_to_event(*ptr);
      if (event >= 0) {
        token.type = LPE_TOKEN_EVENT;
        token.event_mask = (1 << event);
        token.events.push_back(*ptr);
        tokens_.push_back(token);
      }
      else {
        LOG_WARNING << "Invalid character '" << *ptr << "' in LPE expression: " << expression_;
        return false;
      }
      ptr++;
    }
  }

  /* Add end token */
  LPEToken end_token;
  end_token.type = LPE_TOKEN_END;
  end_token.event_mask = 0;
  tokens_.push_back(end_token);

  return true;
}

int LPEParser::char_to_event(char c) const
{
  /* Map characters to event type indices (matching kernel definitions) */
  switch (c) {
    case 'C': return 0;  /* LPE_TYPE_CAMERA */
    case 'R': return 1;  /* LPE_TYPE_REFLECTION */
    case 'T': return 2;  /* LPE_TYPE_TRANSMISSION */
    case 'V': return 3;  /* LPE_TYPE_VOLUME */
    case 'L': return 4;  /* LPE_TYPE_LIGHT */
    case 'O': return 5;  /* LPE_TYPE_EMISSION */
    case 'B': return 6;  /* LPE_TYPE_BACKGROUND */
    /* Scatter types - encode as separate events for simplicity */
    case 'D': return 8;  /* Diffuse scatter */
    case 'G': return 9;  /* Glossy scatter */
    case 'S': return 10; /* Singular scatter */
    default: return -1;
  }
}

bool LPEParser::match(const string &path) const
{
  if (!is_valid_ || tokens_.empty()) {
    return false;
  }

  return match_recursive(path.c_str(), 0);
}

bool LPEParser::match_recursive(const char *path, size_t token_idx) const
{
  /* End of tokens */
  if (token_idx >= tokens_.size()) {
    return *path == '\0';
  }

  const LPEToken &token = tokens_[token_idx];

  /* End token */
  if (token.type == LPE_TOKEN_END) {
    return *path == '\0';
  }

  /* Star operator: match zero or more characters */
  if (token.type == LPE_TOKEN_STAR) {
    /* Try matching zero characters (skip this token) */
    if (match_recursive(path, token_idx + 1)) {
      return true;
    }

    /* Try matching one or more characters */
    const char *p = path;
    while (*p && (token_idx > 0 && char_matches_token(*p, tokens_[token_idx - 1]))) {
      p++;
      if (match_recursive(p, token_idx + 1)) {
        return true;
      }
    }
    return false;
  }

  /* Plus operator: match one or more characters */
  if (token.type == LPE_TOKEN_PLUS) {
    /* Must match at least one character */
    if (*path == '\0' || (token_idx > 0 && !char_matches_token(*path, tokens_[token_idx - 1]))) {
      return false;
    }

    const char *p = path + 1;
    if (match_recursive(p, token_idx + 1)) {
      return true;
    }

    /* Try matching more characters */
    while (*p && (token_idx > 0 && char_matches_token(*p, tokens_[token_idx - 1]))) {
      p++;
      if (match_recursive(p, token_idx + 1)) {
        return true;
      }
    }
    return false;
  }

  /* Optional operator: match zero or one character */
  if (token.type == LPE_TOKEN_OPTIONAL) {
    /* Try matching zero characters */
    if (match_recursive(path, token_idx + 1)) {
      return true;
    }

    /* Try matching one character */
    if (*path && (token_idx > 0 && char_matches_token(*path, tokens_[token_idx - 1]))) {
      return match_recursive(path + 1, token_idx + 1);
    }
    return false;
  }

  /* No more characters in path */
  if (*path == '\0') {
    return false;
  }

  /* Match any single character */
  if (token.type == LPE_TOKEN_ANY) {
    return match_recursive(path + 1, token_idx + 1);
  }

  /* Match specific event or character set */
  if (token.type == LPE_TOKEN_EVENT) {
    if (char_matches_token(*path, token)) {
      return match_recursive(path + 1, token_idx + 1);
    }
    return false;
  }

  return false;
}

bool LPEParser::char_matches_token(char c, const LPEToken &token) const
{
  if (token.type == LPE_TOKEN_ANY) {
    return true;
  }

  if (token.type == LPE_TOKEN_EVENT) {
    int event = char_to_event(c);
    if (event >= 0 && (token.event_mask & (1 << event))) {
      return true;
    }
  }

  return false;
}

CCL_NAMESPACE_END