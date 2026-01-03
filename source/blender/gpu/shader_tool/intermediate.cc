/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 *
 */

#include "intermediate.hh"
#include "scope.hh"
#include "token.hh"
#include "token_stream.hh"

#include <algorithm>
#include <stack>

#if defined(_MSC_VER)
#  define always_inline __forceinline
#else
#  define always_inline inline __attribute__((always_inline))
#endif

namespace blender::gpu::shader::parser {

size_t line_number(const std::string &str, size_t pos)
{
  std::string directive = "#line ";
  /* String to count the number of line. */
  std::string sub_str = str.substr(0, pos);
  size_t nearest_line_directive = sub_str.rfind(directive);
  size_t line_count = 1;
  if (nearest_line_directive != std::string::npos) {
    sub_str = sub_str.substr(nearest_line_directive + directive.size());
    line_count = std::stoll(sub_str) - 1;
  }
  return line_count + std::count(sub_str.begin(), sub_str.end(), '\n');
}

size_t char_number(const std::string &str, size_t pos)
{
  std::string sub_str = str.substr(0, pos);
  size_t nearest_line_directive = sub_str.rfind('\n');
  return (nearest_line_directive == std::string::npos) ?
             (sub_str.size()) :
             (sub_str.size() - nearest_line_directive - 1);
}

std::string line_str(const std::string &str, size_t pos)
{
  size_t start = str.rfind('\n', pos);
  size_t end = str.find('\n', pos);
  start = (start != std::string::npos) ? start + 1 : 0;
  return str.substr(start, end - start);
}

Scope Token::scope() const
{
  if (this->is_invalid()) {
    return Scope::invalid();
  }
  return Scope::from_position(data, data->token_scope[index]);
}

Scope Token::attribute_before() const
{
  if (is_invalid()) {
    return Scope::invalid();
  }
  Token prev = this->prev();
  if (prev == ']' && prev.prev().scope().type() == ScopeType::Attributes) {
    return prev.prev().scope();
  }
  return Scope::invalid();
}

Scope Token::attribute_after() const
{
  if (is_invalid()) {
    return Scope::invalid();
  }
  Token next = this->next();
  if (next == '[' && next.next().scope().type() == ScopeType::Attributes) {
    return next.next().scope();
  }
  return Scope::invalid();
}

struct TokenData {
  std::vector<TokenType> types;
  OffsetIndices offsets;
};

void TokenStream::tokenize()
{
  if (str.empty()) {
    *this = {};
    return;
  }

  TokenData data;

  token_parse(data);
  token_merge(data);

  /* Convert vector of char to string for faster lookups. */
  this->token_types = std::string(reinterpret_cast<char *>(data.types.data()), data.types.size());
  this->token_offsets = std::move(data.offsets);

  token_types_populate();
}

static always_inline TokenType to_type(const char c)
{
  switch (c) {
    case '\n':
      return TokenType::NewLine;
    case ' ':
      return TokenType::Space;
    case '#':
      return TokenType::Hash;
    case '&':
      return TokenType::Ampersand;
    case '^':
      return TokenType::Caret;
    case '|':
      return TokenType::Pipe;
    case '%':
      return TokenType::Percent;
    case '.':
      return TokenType::Dot;
    case '(':
      return TokenType::ParOpen;
    case ')':
      return TokenType::ParClose;
    case '{':
      return TokenType::BracketOpen;
    case '}':
      return TokenType::BracketClose;
    case '[':
      return TokenType::SquareOpen;
    case ']':
      return TokenType::SquareClose;
    case '<':
      return TokenType::AngleOpen;
    case '>':
      return TokenType::AngleClose;
    case '=':
      return TokenType::Assign;
    case '!':
      return TokenType::Not;
    case '*':
      return TokenType::Star;
    case '-':
      return TokenType::Minus;
    case '+':
      return TokenType::Plus;
    case '/':
      return TokenType::Divide;
    case '~':
      return TokenType::Tilde;
    case '\\':
      return TokenType::Backslash;
    case '\"':
      return TokenType::String;
    case '?':
      return TokenType::Question;
    case ':':
      return TokenType::Colon;
    case ',':
      return TokenType::Comma;
    case ';':
      return TokenType::SemiColon;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return TokenType::Number;
    default:
      return TokenType::Word;
  }
}

static always_inline bool always_split_token(const TokenType c)
{
  switch (c) {
    case TokenType::Number:
    case TokenType::Word:
    case TokenType::NewLine:
    case TokenType::Space:
      return false;
    default:
      return true;
  }
}

static const std::array<std::pair<TokenType, bool>, 256> token_table = [] {
  std::array<std::pair<TokenType, bool>, 256> t;
  for (int i = 0; i < 256; ++i) {
    TokenType type = to_type(i);
    t[i] = {type, always_split_token(type)};
  }
  return t;
}();

/* Table lookup variant. Much faster than switch statement.  */
static always_inline std::pair<TokenType, bool> to_type_table(const unsigned char c)
{
  return token_table[c];
}

void TokenStream::token_parse(TokenData &tokens)
{
  /* Reserve space inside the data structures. Allocate 1 token per char as we do not want to
   * resize or check for size inside the hot loop. */
  tokens.types.resize(str.size());
  tokens.offsets.offsets.resize(str.size() + 1);

  TokenType type = TokenType::Invalid;

  TokenType *types_raw = tokens.types.data();
  uint32_t *offsets_raw = tokens.offsets.offsets.data();

  int offset = 0, cursor = 0;
  for (const char c : str) {
    const TokenType prev = type;
    auto [tok_type, always_split] = to_type_table(c);
    type = tok_type;
    /* Its faster to overwrite the previous value with the same value
     * than having a condition. */
    types_raw[cursor] = type;
    offsets_raw[cursor] = offset++;
    /* Split if type mismatch. */
    cursor += (type != prev || always_split);
  }
  /* Set end of last token. */
  offsets_raw[cursor] = offset++;
  /* Resize to the actual usage. */
  tokens.types.resize(cursor);
  tokens.offsets.offsets.resize(cursor + 1);
}

static const std::array<bool, 256> num_literal_table = [] {
  std::array<bool, 256> t;
  for (int c = 0; c < 256; ++c) {
    t[c] = true;
    /* If dot is part of float literal. */
    if (c == '.') {
      continue; /* Merge. */
    }
    /* If 'A-F' is part of hex literal. */
    if (c >= 'A' && c <= 'F') {
      continue; /* Merge. */
    }
    /* If 'a-f' is part of hex literal. */
    /* If 'f' suffix is part of float literal. */
    /* If 'e' is part of float literal. */
    if (c >= 'a' && c <= 'f') {
      continue; /* Merge. */
    }
    /* If 'x' is part of hex literal. */
    if (c == 'x') {
      continue; /* Merge. */
    }
    /* If 'u' is part of unsigned int literal. */
    if (c == 'u') {
      continue; /* Merge. */
    }
    t[c] = false;
  }
  return t;
}();

/* Table lookup variant. Much faster than switch statement.  */
static always_inline bool is_char_part_of_number_literal(const unsigned char c)
{
  return num_literal_table[c];
}

static always_inline bool is_word_part_of_number_literal(const std::string_view str)
{
  for (char c : str) {
    if (!is_char_part_of_number_literal(c)) {
      return false;
    }
  }
  return true;
}

static always_inline bool is_whitespace(TokenType t)
{
  return (t == ' ') || (t == '\n');
}

void TokenStream::token_merge(TokenData &tokens)
{
  const char *str_raw = str.data();
  TokenType *types_raw = tokens.types.data();
  uint32_t *offsets_raw = tokens.offsets.offsets.data();

  /* Never merge the first token. We don't want to loose it. */
  TokenType prev = types_raw[0];

  /* State. */
  bool after_whitespace = is_whitespace(prev);
  bool inside_escaped_char = false;
  bool inside_preprocessor_directive = false;
  bool inside_string = false;
  bool inside_number = false;

  uint32_t cursor = 1;
  for (uint32_t i = 1; i < tokens.types.size(); i++) {
    bool emit = true;
#define merge_if(a) emit &= !(a)

    TokenType tok = types_raw[i];
    uint32_t offset = offsets_raw[i];
    uint32_t tok_size = offsets_raw[i + 1] - offset;

#ifndef NDEBUG
    std::string_view tok_str{str_raw + offset, tok_size};
#endif

    /* Merge string literal. */
    merge_if(inside_string);

    /* Flip flop inside string when finding and unescaped quote. */
    if (tok == String && !inside_escaped_char) {
      inside_string = !inside_string;
    }
    inside_escaped_char = inside_string && (tok == '\\');

    /* Merge number literal. */
    if (inside_number) {
      merge_if((tok == Word || tok == '.') &&
               is_word_part_of_number_literal({str_raw + offset, tok_size}));
      /* If sign is part of float literal after exponent. */
      merge_if((tok == '+' || tok == '-') && str_raw[offset - 1] == 'e');

      /* Disable if we do not emit. */
      inside_number = (tok == Number) || !emit;
    }

    switch (tok) {
      case Hash:
        inside_preprocessor_directive = true;
        break;

      case NewLine:
        after_whitespace = true;
        /* Preprocessor directives. */
        if (inside_preprocessor_directive) {
          /* Detect preprocessor directive newlines `\\\n`. */
          if (prev == Backslash) {
            types_raw[cursor - 1] = PreprocessorNewline;
            continue;
          }
          inside_preprocessor_directive = false;
          /* Make sure to keep the ending newline for a preprocessor directive. */
          break;
        }
        continue;

      case Space:
        after_whitespace = true;
        continue;

      case Word:
        /* Merge words that contain numbers that were split by the tokenizer. */
        if (prev == Word && !after_whitespace) {
          continue;
        }
        break;

      case Number:
        /* If digit is part of word. */
        if (prev == Word && !after_whitespace) {
          continue;
        }
        if (prev == Number) {
          continue;
        }
        inside_number = true;
        break;

      case '=':
        /* Merge '=='. */
        if (prev == '=') {
          types_raw[cursor - 1] = Equal;
          continue;
        }
        /* Merge '!='. */
        if (prev == '!') {
          types_raw[cursor - 1] = NotEqual;
          continue;
        }
        /* Merge '>='. */
        if (prev == '>') {
          types_raw[cursor - 1] = GEqual;
          continue;
        }
        /* Merge '<='. */
        if (prev == '<') {
          types_raw[cursor - 1] = LEqual;
          continue;
        }
        break;

      case '>':
        /* Merge '->'. */
        if (prev == '-') {
          types_raw[cursor - 1] = Deref;
          continue;
        }
        break;

      case '+':
        /* Detect increment. */
        if (prev == '+') {
          types_raw[cursor - 1] = Increment;
          continue;
        }
        break;

      case '-':
        /* Detect decrement. */
        if (prev == '-') {
          types_raw[cursor - 1] = Decrement;
          continue;
        }
        break;

      default:
        break;
    }
    after_whitespace = false;

    if (emit) {
      prev = tok;
      types_raw[cursor] = tok;
      offsets_raw[cursor] = offset;
      cursor += 1;
    }
  }

  tokens.types.resize(cursor);

  tokens.offsets.offsets[cursor] = tokens.offsets.offsets.back();
  tokens.offsets.offsets.resize(cursor + 1);
}

static TokenType type_lookup(std::string_view s)
{
  switch (s.size()) {
    case 2:
      if (s == "do") {
        return Do;
      }
      if (s == "if") {
        return If;
      }
      break;
    case 3:
      if (s == "for") {
        return For;
      }
      break;
    case 4:
      if (s == "case") {
        return Case;
      }
      if (s == "else") {
        return Else;
      }
      if (s == "enum") {
        return Enum;
      }
      if (s == "this") {
        return This;
      }
      break;
    case 5:
      if (s == "break") {
        return Break;
      }
      if (s == "class") {
        return Class;
      }
      if (s == "const") {
        return Const;
      }
      if (s == "union") {
        return Union;
      }
      if (s == "using") {
        return Using;
      }
      if (s == "while") {
        return While;
      }
      break;
    case 6:
      if (s == "inline") {
        return Inline;
      }
      if (s == "public") {
        return Public;
      }
      if (s == "return") {
        return Return;
      }
      if (s == "static") {
        return Static;
      }
      if (s == "struct") {
        return Struct;
      }
      if (s == "switch") {
        return Switch;
      }
      break;
    case 7:
      if (s == "private") {
        return Private;
      }
      break;
    case 8:
      if (s == "continue") {
        return Continue;
      }
      if (s == "template") {
        return Template;
      }
      break;
    case 9:
      if (s == "constexpr") {
        return Constexpr;
      }
      if (s == "namespace") {
        return Namespace;
      }
      break;
  }
  return Invalid;
}

void TokenStream::token_types_populate()
{
  int tok_id = -1;
  for (char &c : token_types) {
    tok_id++;
    if (TokenType(c) == Word) {
      IndexRange range = token_offsets[tok_id];
      std::string_view word(str.data() + range.start, range.size);

      size_t last_non_whitespace = word.find_last_not_of(" \n");
      if (last_non_whitespace == std::string::npos) {
        continue;
      }

      word.remove_suffix(word.size() - last_non_whitespace - 1);

      TokenType type = type_lookup(word);
      if (type != Invalid) {
        c = type;
      }
    }
  }
}

void TokenStream::parse_scopes(report_callback &report_error)
{
  scope_parse(report_error);
  scope_token_populate();
}

void TokenStream::scope_parse(report_callback &report_error)
{
  {
    /* Scope detection. */
    scope_ranges.clear();
    scope_types.clear();

    size_t predicted_scope_count = token_types.size() / 2;
    scope_ranges.reserve(predicted_scope_count);
    scope_types.reserve(predicted_scope_count);

    struct ScopeItem {
      ScopeType type;
      size_t start;
      int index;
    };

    int scope_index = 0;
    std::stack<ScopeItem> scopes;

    auto enter_scope = [&](ScopeType type, size_t start_tok_id) {
      scopes.emplace(ScopeItem{type, start_tok_id, scope_index++});
      scope_ranges.emplace_back(start_tok_id, 1);
      scope_types += char(type);
    };

    auto exit_scope = [&](int end_tok_id) {
      if (scopes.empty()) {
        return;
      }
      ScopeItem scope = scopes.top();
      scope_ranges[scope.index].size = end_tok_id - scope.start + 1;
      scopes.pop();
    };

    enter_scope(ScopeType::Global, 0);

    int in_template = 0;

    int tok_id = -1;
    for (const char &c : token_types) {
      tok_id++;

      if (scopes.top().type == ScopeType::Preprocessor) {
        if (TokenType(c) == NewLine) {
          exit_scope(tok_id);
        }
        else {
          /* Do nothing. Enclose all preprocessor lines together. */
          continue;
        }
      }

      switch (TokenType(c)) {
        case Hash:
          enter_scope(ScopeType::Preprocessor, tok_id);
          break;
        case Assign:
          if (scopes.top().type == ScopeType::Assignment) {
            /* Chained assignments. */
            exit_scope(tok_id - 1);
          }
          enter_scope(ScopeType::Assignment, tok_id);
          break;
        case BracketOpen: {
          /* Scan back identifier that could contain namespaces. */
          TokenType keyword;
          int pos = 2;
          do {
            keyword = (tok_id >= pos) ? TokenType(token_types[tok_id - pos]) : TokenType::Invalid;
            pos += 3;
          } while (keyword != Invalid && keyword == Colon);

          /* Skip host_shared attribute for structures if any. */
          if (keyword == ']') {
            keyword = (tok_id >= pos) ? TokenType(token_types[tok_id - pos]) : TokenType::Invalid;
            if (keyword == '[') {
              pos += 2;
              keyword = (tok_id >= pos) ? TokenType(token_types[tok_id - pos]) :
                                          TokenType::Invalid;
            }
          }

          if (keyword == Struct || keyword == Class) {
            enter_scope(ScopeType::Struct, tok_id);
          }
          else if (keyword == Enum) {
            enter_scope(ScopeType::Local, tok_id);
          }
          else if (keyword == Namespace) {
            enter_scope(ScopeType::Namespace, tok_id);
          }
          else if (scopes.top().type == ScopeType::Global) {
            enter_scope(ScopeType::Function, tok_id);
          }
          else if (scopes.top().type == ScopeType::Struct) {
            enter_scope(ScopeType::Function, tok_id);
          }
          else if (scopes.top().type == ScopeType::Namespace) {
            enter_scope(ScopeType::Function, tok_id);
          }
          else {
            enter_scope(ScopeType::Local, tok_id);
          }
          break;
        }
        case ParOpen:
          if ((tok_id >= 1 && token_types[tok_id - 1] == For) ||
              (tok_id >= 1 && token_types[tok_id - 1] == While))
          {
            enter_scope(ScopeType::LoopArgs, tok_id);
          }
          else if (tok_id >= 1 && token_types[tok_id - 1] == Switch) {
            enter_scope(ScopeType::SwitchArg, tok_id);
          }
          else if (scopes.top().type == ScopeType::Global) {
            enter_scope(ScopeType::FunctionArgs, tok_id);
          }
          else if (scopes.top().type == ScopeType::Struct) {
            enter_scope(ScopeType::FunctionArgs, tok_id);
          }
          else if ((scopes.top().type == ScopeType::Function ||
                    scopes.top().type == ScopeType::Local ||
                    scopes.top().type == ScopeType::Attribute) &&
                   (tok_id >= 1 && token_types[tok_id - 1] == Word))
          {
            enter_scope(ScopeType::FunctionCall, tok_id);
          }
          else {
            enter_scope(ScopeType::Local, tok_id);
          }
          break;
        case SquareOpen:
          if (tok_id >= 1 && token_types[tok_id - 1] == SquareOpen) {
            enter_scope(ScopeType::Attributes, tok_id);
          }
          else {
            enter_scope(ScopeType::Subscript, tok_id);
          }
          break;
        case AngleOpen:
          if (tok_id >= 1) {
            char prev_char = str[token_offsets[tok_id - 1].last()];
            /* Rely on the fact that template are formatted without spaces but comparison isn't. */
            if ((prev_char != ' ' && prev_char != '\n' && prev_char != '<') ||
                token_types[tok_id - 1] == Template)
            {
              enter_scope(ScopeType::Template, tok_id);
              in_template++;
            }
          }
          break;
        case AngleClose:
          if (scopes.top().type == ScopeType::Assignment && in_template > 0) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::TemplateArg) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::Template) {
            exit_scope(tok_id);
            in_template--;
          }
          break;
        case BracketClose:
          if (scopes.top().type == ScopeType::Assignment) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::Struct || scopes.top().type == ScopeType::Local ||
              scopes.top().type == ScopeType::Namespace ||
              scopes.top().type == ScopeType::LoopBody ||
              scopes.top().type == ScopeType::SwitchBody ||
              scopes.top().type == ScopeType::Function || scopes.top().type == ScopeType::Function)
          {
            exit_scope(tok_id);
          }
          else {
            Token token = Token::from_position(this, tok_id);
            report_error(token.line_number(),
                         token.char_number(),
                         token.line_str(),
                         "Unexpected '}' token");
            /* Avoid out of bound access for the rest of the processing. Empty everything. */
            *this = {};
            return;
          }
          break;
        case ParClose:
          if (scopes.top().type == ScopeType::Assignment) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::FunctionArg) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::FunctionParam) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::LoopArg) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::LoopArgs ||
              scopes.top().type == ScopeType::SwitchArg ||
              scopes.top().type == ScopeType::FunctionArgs ||
              scopes.top().type == ScopeType::FunctionCall ||
              scopes.top().type == ScopeType::Local)
          {
            exit_scope(tok_id);
          }
          else {
            Token token = Token::from_position(this, tok_id);
            report_error(token.line_number(),
                         token.char_number(),
                         token.line_str(),
                         "Unexpected ')' token");
            /* Avoid out of bound access for the rest of the processing. Empty everything. */
            *this = {};
            return;
          }
          break;
        case SquareClose:
          if (scopes.top().type == ScopeType::Attribute) {
            exit_scope(tok_id - 1);
          }
          exit_scope(tok_id);
          break;
        case SemiColon:
          if (scopes.top().type == ScopeType::Assignment) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::FunctionArg) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::TemplateArg) {
            exit_scope(tok_id - 1);
          }
          if (scopes.top().type == ScopeType::LoopArg) {
            exit_scope(tok_id - 1);
          }
          break;
        case Comma:
          if (scopes.top().type == ScopeType::Assignment) {
            exit_scope(tok_id - 1);
          }
          switch (scopes.top().type) {
            case ScopeType::FunctionArg:
            case ScopeType::FunctionParam:
            case ScopeType::TemplateArg:
            case ScopeType::Attribute:
              exit_scope(tok_id - 1);
              break;
            default:
              break;
          }
          break;
        default:
          switch (scopes.top().type) {
            case ScopeType::Attributes:
              enter_scope(ScopeType::Attribute, tok_id);
              break;
            case ScopeType::FunctionArgs:
              enter_scope(ScopeType::FunctionArg, tok_id);
              break;
            case ScopeType::FunctionCall:
              enter_scope(ScopeType::FunctionParam, tok_id);
              break;
            case ScopeType::LoopArgs:
              enter_scope(ScopeType::LoopArg, tok_id);
              break;
            case ScopeType::Template:
              enter_scope(ScopeType::TemplateArg, tok_id);
              break;
            default:
              break;
          }
          break;
      }
    }

    if (scopes.empty()) {
      Token token = Token::from_position(this, tok_id);
      report_error(token.line_number(),
                   token.char_number(),
                   token.line_str(),
                   "Extraneous end of scope somewhere in that file");

      /* Avoid out of bound access for the rest of the processing. Empty everything. */
      *this = {};
      return;
    }

    if (scopes.top().type == ScopeType::Preprocessor) {
      exit_scope(tok_id - 1);
    }

    if (scopes.top().type != ScopeType::Global) {
      ScopeItem scope_item = scopes.top();
      Token token = Token::from_position(this, scope_ranges[scope_item.index].start);
      report_error(
          token.line_number(), token.char_number(), token.line_str(), "Unterminated scope");

      /* Avoid out of bound access for the rest of the processing. Empty everything. */
      *this = {};
      return;
    }

    exit_scope(tok_id);
  }
}

void TokenStream::scope_token_populate()
{
  token_scope.clear();
  token_scope.resize(scope_ranges[0].size);

  std::stack<uint32_t> stack;

  int scope_id = 0;
  for (const IndexRange &range : scope_ranges) {
    std::fill(token_scope.begin() + range.start,
              token_scope.begin() + range.start + range.size,
              scope_id);
    scope_id++;
  }
}

/* Return true if any mutation was applied. */
bool IntermediateForm::only_apply_mutations()
{
  if (mutations_.empty()) {
    return false;
  }

  /* Order mutations so that they can be applied in one pass. */
  std::stable_sort(mutations_.begin(), mutations_.end());

  /* Make sure to pad the input string in case of insertion after the last char. */
  bool added_trailing_new_line = false;
  if (data_.str.back() != '\n') {
    data_.str += '\n';
    added_trailing_new_line = true;
  }

  std::string result;
  result.reserve(data_.str.size());

  int64_t offset = 0;
  for (const Mutation &mut : mutations_) {
    size_t start = mut.src_range.start;
    size_t end = start + mut.src_range.size;
    /* Copy unchanged text. */
    result.append(data_.str.data() + offset, start - offset);
    /* Append replacement. */
    result.append(mut.replacement);
    offset = end;
  }
  result.append(data_.str.data() + offset, data_.str.size() - offset);

  data_.str = std::move(result);

  mutations_.clear();

  if (added_trailing_new_line) {
    data_.str.pop_back();
  }
  return true;
}

}  // namespace blender::gpu::shader::parser
