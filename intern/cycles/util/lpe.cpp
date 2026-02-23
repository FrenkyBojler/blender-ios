/* SPDX-FileCopyrightText: 2011-2024 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "util/lpe.h"
#include "util/log.h"

CCL_NAMESPACE_BEGIN

LPEParser::LPEParser() : is_valid_(false), has_negation_(false) {}

LPEParser::~LPEParser() {}

bool LPEParser::compile(const string &expression)
{
  expression_ = expression;
  tokens_.clear();
  patterns_.clear();
  operators_.clear();
  has_negation_ = false;

  if (expression.empty()) {
    is_valid_ = false;
    return false;
  }

  /* Check for leading negation operator ! */
  const char *ptr = expression.c_str();
  if (*ptr == '!') {
    has_negation_ = true;
    ptr++;
    while (*ptr == ' ')
      ptr++; /* Skip spaces after ! */
  }

  string expr_without_negation = has_negation_ ? string(ptr) : expression;

  /* Split expression by | and - operators (with surrounding spaces) */
  vector<string> parts;
  string current;
  ptr = expr_without_negation.c_str();

  while (*ptr) {
    /* Check for operator with spaces: " | " or " - " */
    if (*ptr == ' ' && *(ptr + 1) != '\0' && *(ptr + 2) == ' ') {
      char op = *(ptr + 1);
      if (op == '|' || op == '-' || op == '+') {
        parts.push_back(current);
        operators_.push_back(op == '-' ? LPE_OP_SUBTRACT : LPE_OP_OR);
        current.clear();
        ptr += 3; /* Skip " op " */
        continue;
      }
    }
    current += *ptr;
    ptr++;
  }
  parts.push_back(current);

  /* Parse each sub-pattern */
  for (size_t i = 0; i < parts.size(); i++) {
    LPEPattern pattern;
    if (!parse_pattern(parts[i], pattern)) {
      is_valid_ = false;
      return false;
    }
    patterns_.push_back(pattern);
  }

  /* For backward compatibility: if single pattern, populate tokens_ */
  if (patterns_.size() == 1 && !has_negation_) {
    tokens_ = patterns_[0].tokens;
  }

  is_valid_ = true;
  return true;
}

bool LPEParser::parse_pattern(const string &pattern_str, LPEPattern &pattern)
{
  pattern.pattern_str = pattern_str;
  pattern.tokens.clear();

  const char *ptr = pattern_str.c_str();

  while (*ptr) {
    /* Skip spaces */
    if (*ptr == ' ') {
      ptr++;
      continue;
    }

    LPEToken token;
    token.event_mask = 0;
    token.is_negated = false;

    /* Check for wildcards */
    if (*ptr == '*') {
      token.type = LPE_TOKEN_STAR;
      pattern.tokens.push_back(token);
      ptr++;
      continue;
    }
    else if (*ptr == '+') {
      token.type = LPE_TOKEN_PLUS;
      pattern.tokens.push_back(token);
      ptr++;
      continue;
    }
    else if (*ptr == '?') {
      token.type = LPE_TOKEN_OPTIONAL;
      pattern.tokens.push_back(token);
      ptr++;
      continue;
    }
    else if (*ptr == '.') {
      token.type = LPE_TOKEN_ANY;
      pattern.tokens.push_back(token);
      ptr++;
      continue;
    }
    /* Character sets [DGS] or negated [^DGS] or tags [^'label'] */
    else if (*ptr == '[') {
      ptr++;
      token.type = LPE_TOKEN_EVENT;

      /* Check for negation */
      if (*ptr == '^') {
        token.is_negated = true;
        ptr++;
      }

      /* Check if this is a tag in character set [^'label'] */
      if (*ptr == '\'') {
        ptr++; /* Skip opening ' */

        /* Check for wildcard '*' */
        if (*ptr == '*' && *(ptr + 1) == '\'') {
          token.tag.is_wildcard = true;
          token.tag.is_negated = token.is_negated;
          ptr += 2; /* Skip '* */
        }
        else {
          /* Parse tag content until closing ' */
          string tag_content;
          while (*ptr && *ptr != '\'') {
            tag_content += *ptr;
            ptr++;
          }

          if (*ptr == '\'') {
            ptr++; /* Skip closing ' */
          }

          /* Parse type:name or just name */
          size_t colon = tag_content.find(':');
          if (colon != string::npos) {
            string type_str = tag_content.substr(0, colon);
            token.tag.name = tag_content.substr(colon + 1);

            if (type_str == "lightgroup" || type_str == "lgroup" || type_str == "lgp") {
              token.tag.type = LPE_TAG_LIGHT_GROUP;
            }
            else if (type_str == "object" || type_str == "obj") {
              token.tag.type = LPE_TAG_OBJECT;
            }
            else if (type_str == "material" || type_str == "mat") {
              token.tag.type = LPE_TAG_MATERIAL;
            }
            else {
              LOG_WARNING << "Unknown tag type '" << type_str
                          << "' in LPE pattern: " << pattern_str;
            }
          }
          else {
            /* No prefix: this is a tag name, type will be inferred from context */
            token.tag.name = tag_content;
            token.tag.type = LPE_TAG_NONE; /* Will be inferred during matching */
          }

          token.tag.is_negated = token.is_negated;
        }

        /* For tags in character sets, match all event types */
        const int all_events_mask = (1 << 0) |  /* C */
                                    (1 << 1) |  /* R */
                                    (1 << 2) |  /* T */
                                    (1 << 3) |  /* V */
                                    (1 << 4) |  /* L */
                                    (1 << 5) |  /* O */
                                    (1 << 6) |  /* B */
                                    (1 << 8) |  /* D */
                                    (1 << 9) |  /* G */
                                    (1 << 10) | /* S */
                                    (1 << 11) | /* s */
                                    (1 << 12);  /* A */
        token.event_mask = all_events_mask;
        token.is_negated = false; /* Negation is on the tag, not the event mask */
      }
      else {
        /* Regular character set with event types */
        while (*ptr && *ptr != ']') {
          int event = char_to_event(*ptr);
          if (event >= 0) {
            token.event_mask |= (1 << event);
            token.events.push_back(*ptr);
          }
          ptr++;
        }

        /* Invert mask if negated */
        if (token.is_negated && token.event_mask != 0) {
          /* Create mask with all valid event bits set */
          const int all_events_mask = (1 << 0) |  /* C */
                                      (1 << 1) |  /* R */
                                      (1 << 2) |  /* T */
                                      (1 << 3) |  /* V */
                                      (1 << 4) |  /* L */
                                      (1 << 5) |  /* O */
                                      (1 << 6) |  /* B */
                                      (1 << 8) |  /* D */
                                      (1 << 9) |  /* G */
                                      (1 << 10) | /* S */
                                      (1 << 11) | /* s */
                                      (1 << 12);  /* A */
          token.event_mask = all_events_mask & ~token.event_mask;
        }
      }

      if (*ptr == ']') {
        ptr++;
      }

      if (token.event_mask != 0 || token.tag.type != LPE_TAG_NONE || token.tag.is_wildcard) {
        pattern.tokens.push_back(token);
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
        ptr++;

        /* Check for tag filter 'type:name' or 'name' */
        if (*ptr == '\'') {
          ptr++;

          /* Check for wildcard '*' */
          if (*ptr == '*' && *(ptr + 1) == '\'') {
            token.tag.is_wildcard = true;
            ptr += 2;
          }
          else {
            /* Parse tag content until closing ' */
            string tag_content;
            while (*ptr && *ptr != '\'') {
              tag_content += *ptr;
              ptr++;
            }

            if (*ptr == '\'') {
              ptr++;
            }

            /* Parse type:name or just name */
            size_t colon = tag_content.find(':');
            if (colon != string::npos) {
              string type_str = tag_content.substr(0, colon);
              token.tag.name = tag_content.substr(colon + 1);

              if (type_str == "lightgroup" || type_str == "lgroup" || type_str == "lgp") {
                token.tag.type = LPE_TAG_LIGHT_GROUP;
              }
              else if (type_str == "object" || type_str == "obj") {
                token.tag.type = LPE_TAG_OBJECT;
              }
              else if (type_str == "material" || type_str == "mat") {
                token.tag.type = LPE_TAG_MATERIAL;
              }
              else {
                LOG_WARNING << "Unknown tag type '" << type_str
                            << "' in LPE pattern: " << pattern_str;
              }
            }
            else {
              /* No prefix: assume light group for L tokens, object for others */
              token.tag.name = tag_content;
              char event_char = token.events.empty() ? '\0' : token.events[0];
              token.tag.type = (event_char == 'L') ? LPE_TAG_LIGHT_GROUP : LPE_TAG_OBJECT;
            }
          }
        }

        pattern.tokens.push_back(token);
      }
      else {
        LOG_WARNING << "Invalid character '" << *ptr << "' in LPE pattern: " << pattern_str;
        return false;
      }
    }
  }

  /* Add end token */
  LPEToken end_token;
  end_token.type = LPE_TOKEN_END;
  end_token.event_mask = 0;
  end_token.is_negated = false;
  pattern.tokens.push_back(end_token);

  return true;
}

int LPEParser::char_to_event(char c) const
{
  /* Map characters to event type indices (matching kernel definitions) */
  switch (c) {
    case 'C':
      return 0; /* LPE_TYPE_CAMERA */
    case 'R':
      return 1; /* LPE_TYPE_REFLECTION */
    case 'T':
      return 2; /* LPE_TYPE_TRANSMISSION */
    case 'V':
      return 3; /* LPE_TYPE_VOLUME */
    case 'L':
      return 4; /* LPE_TYPE_LIGHT */
    case 'O':
      return 5; /* LPE_TYPE_EMISSION */
    case 'B':
      return 6; /* LPE_TYPE_BACKGROUND */
    /* Scatter types - encode as separate events for simplicity */
    case 'D':
      return 8; /* Diffuse scatter */
    case 'G':
      return 9; /* Glossy scatter */
    case 'S':
      return 10; /* Singular scatter */
    case 's':
      return 11; /* Straight/transparent transmission */
    case 'A':
      return 12; /* Albedo */
    default:
      return -1;
  }
}

bool LPEParser::match(const string &path) const
{
  if (!is_valid_) {
    return false;
  }

  /* For simple single-pattern expressions (backward compatibility) */
  if (patterns_.empty() && !tokens_.empty()) {
    return match_recursive(path.c_str(), 0);
  }

  /* For multi-pattern expressions with operators */
  if (patterns_.empty()) {
    return false;
  }

  /* Match first pattern */
  bool result = match_pattern(path.c_str(), patterns_[0]);

  /* Apply operators */
  for (size_t i = 0; i < operators_.size(); i++) {
    bool next = match_pattern(path.c_str(), patterns_[i + 1]);

    switch (operators_[i]) {
      case LPE_OP_OR:
        result = result || next; /* Union */
        break;
      case LPE_OP_SUBTRACT:
        result = result && !next; /* Difference */
        break;
      default:
        break;
    }
  }

  /* Apply negation if present */
  if (has_negation_) {
    result = !result;
  }

  return result;
}

bool LPEParser::match_pattern(const char *path, const LPEPattern &pattern) const
{
  if (pattern.tokens.empty()) {
    return false;
  }

  return match_recursive(path, 0, pattern.tokens);
}

bool LPEParser::match_recursive(const char *path, size_t token_idx) const
{
  return match_recursive(path, token_idx, tokens_);
}

bool LPEParser::match_recursive(const char *path,
                                size_t token_idx,
                                const vector<LPEToken> &tokens) const
{
  /* End of tokens */
  if (token_idx >= tokens.size()) {
    return *path == '\0';
  }

  const LPEToken &token = tokens[token_idx];

  /* End token */
  if (token.type == LPE_TOKEN_END) {
    return *path == '\0';
  }

  /* Star operator: match zero or more characters */
  if (token.type == LPE_TOKEN_STAR) {
    /* Try matching zero characters (skip this token) */
    if (match_recursive(path, token_idx + 1, tokens)) {
      return true;
    }

    /* Try matching one or more characters */
    const char *p = path;
    while (*p && (token_idx > 0 && char_matches_token(*p, tokens[token_idx - 1]))) {
      p++;
      if (match_recursive(p, token_idx + 1, tokens)) {
        return true;
      }
    }
    return false;
  }

  /* Plus operator: match one or more characters */
  if (token.type == LPE_TOKEN_PLUS) {
    /* Must match at least one character */
    if (*path == '\0' || (token_idx > 0 && !char_matches_token(*path, tokens[token_idx - 1]))) {
      return false;
    }

    const char *p = path + 1;
    if (match_recursive(p, token_idx + 1, tokens)) {
      return true;
    }

    /* Try matching more characters */
    while (*p && (token_idx > 0 && char_matches_token(*p, tokens[token_idx - 1]))) {
      p++;
      if (match_recursive(p, token_idx + 1, tokens)) {
        return true;
      }
    }
    return false;
  }

  /* Optional operator: match zero or one character */
  if (token.type == LPE_TOKEN_OPTIONAL) {
    /* Try matching zero characters */
    if (match_recursive(path, token_idx + 1, tokens)) {
      return true;
    }

    /* Try matching one character */
    if (*path && (token_idx > 0 && char_matches_token(*path, tokens[token_idx - 1]))) {
      return match_recursive(path + 1, token_idx + 1, tokens);
    }
    return false;
  }

  /* No more characters in path */
  if (*path == '\0') {
    return false;
  }

  /* Match any single character */
  if (token.type == LPE_TOKEN_ANY) {
    return match_recursive(path + 1, token_idx + 1, tokens);
  }

  /* Match specific event or character set */
  if (token.type == LPE_TOKEN_EVENT) {
    if (char_matches_token(*path, token)) {
      return match_recursive(path + 1, token_idx + 1, tokens);
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
