/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "shader_tool/expression.hh"
#include "shader_tool/intermediate.hh"

#include "gpu_shader_private.hh"

namespace blender::gpu {

using namespace shader::parser;

/* Fast C (incomplete) preprocessor implementation.  */
struct Preprocessor {
  IntermediateForm &parser;

  struct TokenRange {
    Token start, end;
  };

  Vector<Token, 8> jump_stack;
  Map<StringRef, Token> defines;
  Set<StringRef> visited_macros;

  enum DirectiveType {
    /* Any other unhandled directives (warnings / errors / pragma etc...). */
    Other,
    Define,
    Undef,
    Line,
    If,
    Ifdef,
    Ifndef,
    Elif,
    Else,
    Endif,
  };

  static DirectiveType to_directive_type(const StringRef str)
  {
    if (str.size() < 2) {
      return Other;
    }
    /* Switch on the second character as there is no overlap
     * between "Other" and the directives we care about.  */
    switch (str[1]) {
      default:
      case 'x': /* extension */
      case 'a': /* warning */
      case 'r': /* error, pragma */
        return Other;
      case 'e': /* define, version */
        return str[0] == 'd' ? Define : Other;
      case 'n': /* undef, endif */
        return str[0] == 'u' ? Undef : Endif;
      case 'i': /* line */
        return Line;
      case 'l': /* else, elif */
        return str[2] == 'i' ? Elif : Else;
      case 'f': /* if, ifdef, ifndef */
        switch (str.size()) {
          default:
          case 2:
            return If;
          case 5:
            return Ifdef;
          case 6:
            return Ifndef;
        }
    }
  }

  static StringRef str(const Token t)
  {
    /* Note: Whitespaces where not merged (because of TokenizePreprocessor), so using
     * str_view_with_whitespace will be faster.  */
    return t.str_view_with_whitespace();
  }

  static StringRef str(const TokenRange &range)
  {
    /* Note: Whitespaces where not merged (because of TokenizePreprocessor), so using
     * str_view_with_whitespace will be faster.  */
    StringRef start = range.start.str_view_with_whitespace();
    StringRef end = range.end.str_view_with_whitespace();
    return StringRef(start.data(), end.data() + end.size());
  }

  std::string new_lines(const Token start, const Token end)
  {
    std::string_view dir_str = parser.substr_range_inclusive_view(start, end);
    int line_count = std::count(dir_str.begin(), dir_str.end(), '\n');
    return std::string(line_count, '\n');
  }

  static Token end_of_directive(const Token dir_tok)
  {
    Token tok = dir_tok;

    while (tok != NewLine) {
      if (tok.next() == Invalid) {
        /* Error or end of file. */
        return tok;
      }
      tok = skip_directive_newlines(tok.next());
    }
    return tok.prev();
  }

  static Token skip_space(Token tok)
  {
    while (tok == Space) {
      tok = tok.next();
    }
    return tok;
  }

  static Token skip_space_backward(Token tok)
  {
    while (tok == Space) {
      tok = tok.prev();
    }
    return tok;
  }

  static Token skip_directive_newlines(Token tok)
  {
    while (tok == '\\' && tok.next() == '\n') {
      tok = tok.next().next();
    }
    return tok;
  }

  static Token directive_identifier_token(Token hash_token)
  {
    return skip_space(hash_token.next());
  }

  static StringRef directive_identifier(Token hash_token)
  {
    return str(directive_identifier_token(hash_token));
  }

  static Token get_end_of_parameter(Token tok, bool skip_to_end = false)
  {
    /* Avoid matching comma inside parameter function calls. */
    int stack = 1;
    tok = tok.next();
    while (tok.is_valid()) {
      if (tok == '(') {
        stack++;
      }
      else if (tok == ')') {
        stack--;
      }
      if (stack == 0) {
        return tok;
      }
      if (stack == 1 && tok == ',' && !skip_to_end) {
        return tok;
      }
      tok = tok.next();
    }
    return tok;
  }

  /* Try to match the token pointed at by cursor with a defined macro.
   * If that happen advance the cursor to the end of the macro (in case of functional macro). */
  void try_expand(IntermediateForm &parser, const TokenStream &data, int &cursor)
  {
    Token tok = Token::from_position(&data, cursor);
    Token macro_tok = defines.lookup_default(str(tok), Token::invalid());
    if (macro_tok.is_valid()) {
      auto [replacement, end] = expand_macro(tok, macro_tok);
      parser.replace(tok, end, replacement);
      cursor = end.index;
    }
  }

  /* Parse and expand with the current set of macro identifier. */
  std::string parse_and_expand(StringRef input)
  {
    if (input.is_empty()) {
      return "";
    }
    report_callback report = [](int, int, std::string, const char *) {};
    IntermediateForm parser(input, report, ParserStage::TokenizePreprocessor, false);

    const TokenStream &data = parser.data_get();

    for (int cursor = 0; cursor < data.token_types.size(); cursor++) {
      TokenType tok_type = TokenType(data.token_types[cursor]);
      if (tok_type == Word) {
        try_expand(parser, data, cursor);
      }
    }
    return parser.result_get();
  }

  struct ExpandedResult {
    /* Replacement content. */
    std::string str;
    /* End of range to replace. */
    Token end_of_expansion;
  };

  ExpandedResult expand_macro(Token expanded_tok, Token macro_name)
  {
    Token tok = macro_name.next();
    const bool is_function = (tok == '(');

    Token end_of_expansion = expanded_tok;

    tok = skip_space(tok);

    /* Empty definition. */
    if (tok == '\n') {
      return {"", end_of_expansion};
    }

    StringRef macro_name_str = str(macro_name);
    if (visited_macros.contains(macro_name_str)) {
      /* Recursion. Do not expand. Still replace by the original token. */
      return {macro_name_str, end_of_expansion};
    }

    Map<StringRef, TokenRange> macro_parameters;
    if (is_function) {
      /* This is a functional macro. */

      Token param = skip_space(expanded_tok.next());
      if (param != '(') {
        /* Macro doesn't have parameters. It should not expand. */
        visited_macros.remove(macro_name_str);
        return {macro_name_str, end_of_expansion};
      }

      /* Parse parameters & arguments. */
      macro_parameters.clear_and_keep_capacity();
      while (tok != ')') {
        /* Continue to the next name. */
        tok = skip_space(tok.next());
        if (tok == ')') {
          /* Function with no arguments. */
          param = get_end_of_parameter(param);
          if (param == Invalid) {
            /* Error: missing closing parenthesis. */
            /* Cancel expansion. */
            return {macro_name_str, expanded_tok};
          }
          if (param != ')') {
            /* Error: too many arguments provided to function-like macro invocation. */
            /* Cancel expansion. */
            return {macro_name_str, expanded_tok};
          }
          break;
        }

        Token param_start = param;
        Token param_end = get_end_of_parameter(param_start);

        StringRef argument_name = str(tok);
        if (argument_name == "...") {
          param_end = get_end_of_parameter(param_start, true);
          argument_name = "__VA_ARGS__";
        }

        /* If there is only token for parameters (it could be empty string). */
        if (param_start.next() == param_end.prev()) {
          macro_parameters.add(argument_name, {param_start.next(), param_start.next()});
        }
        else {
          macro_parameters.add(
              argument_name,
              {skip_space(param_start.next()), skip_space_backward(param_end.prev())});
        }

        /* Continue to the next separator. */
        tok = skip_space(tok.next());
        param = param_end;

        if (tok == Invalid) {
          break;
        }
      }
      /* Skip closing parenthesis. */
      tok = skip_space(tok.next());
      /* Make sure to replace the whole call. */
      end_of_expansion = param;
    }

    std::string expanded;
    expanded.reserve(256);

    bool prev_is_concat = false;
    while (tok != NewLine) {
      if (tok == '#' && tok.next() == '#') {
        /* Token concat. */
        tok = tok.next().next();
        continue;
      }

      if (tok == '\\' && tok.next() == '\n') {
        /* Preprocessor new line. Skip and continue. */
        tok = tok.next().next();
        /* Still insert a space to avoid merging tokens. */
        expanded += ' ';
        continue;
      }

      if (tok == Invalid) {
        /* Error. */
        break;
      }

      bool next_is_concat = (tok == '#' && tok.next() == '#');

      if (tok == ' ') {
        /* Replace multiple spaces by only one. Shrinks final codebase. */
        expanded += ' ';
      }
      else if (tok == Word) {
        bool replaced = false;

        if (is_function && !next_is_concat && !prev_is_concat) {
          /* Lookup macro arguments. */
          TokenRange *macro_value_ptr = macro_parameters.lookup_ptr(str(tok));
          if (macro_value_ptr) {
            TokenRange &macro_value = *macro_value_ptr;

            /* Expand argument. Can expand to the same macro (finite recursion). */
            expanded += parse_and_expand(str(macro_value));
            replaced = true;
          }
        }

        if (!replaced) {
          /* Fallback to no expansion. */
          expanded += str(tok);
        }
      }
      else {
        expanded += str(tok);
      }

      prev_is_concat = next_is_concat;
      tok = tok.next();
    }

    /* Add to the set to avoid infinite recursion. */
    visited_macros.add(macro_name_str);

    expanded = parse_and_expand(expanded);

    visited_macros.remove(macro_name_str);
    return {expanded, end_of_expansion};
  }

  /* Returns the hash token. */
  static Token find_next_conditional_directive(Token hash_tok)
  {
    while (true) {
      hash_tok = hash_tok.find_next(Hash);
      if (hash_tok == Invalid) {
        BLI_assert_unreachable();
        return hash_tok;
      }
      StringRef dir_str = directive_identifier(hash_tok);
      if (ELEM(dir_str, "if", "ifdef", "ifndef", "else", "elif", "endif")) {
        return hash_tok;
      }
    }
    BLI_assert_unreachable();
    return Token::invalid();
  }

  /* Returns the hash token. */
  static Token find_next_matching_conditional_directive(Token hash_tok)
  {
    int stack = 1;
    Token tok = hash_tok;
    while (tok.is_valid()) {
      tok = find_next_conditional_directive(tok);
      if (tok.next() == Invalid) {
        break;
      }
      StringRef dir_str = directive_identifier(tok);
      if (ELEM(dir_str, "if", "ifdef", "ifndef")) {
        stack++;
      }
      else if (ELEM(dir_str, "endif")) {
        stack--;
      }

      if (stack == 0) {
        return tok;
      }
      if (stack == 1 && ELEM(dir_str, "else", "elif")) {
        return tok;
      }
    }
    BLI_assert_unreachable();
    return Token::invalid();
  }

  bool evaluate_expression(const Token start, const Token end)
  {
    /* Expand expression into integer ops string. */
    std::string expand;
    expand.reserve(256);

    Token tok = start;
    while (true) {
      StringRef tok_str = str(tok);

      Token macro_tok = defines.lookup_default(tok_str, Token::invalid());
      if (macro_tok.is_valid()) {
        auto [replacement, macro_end] = expand_macro(tok, macro_tok);
        expand += replacement;
        tok = macro_end;
      }
      else if (tok_str == "defined") {
        /* Parenthesis or space */
        tok = skip_space(tok.next());
        const bool is_function = (tok == '(');
        /* Token to search. */
        if (is_function) {
          tok = skip_space(tok.next());
        }
        expand += (defines.contains(str(tok)) ? "1" : "0");
        if (is_function) {
          /* End parenthesis. */
          tok = skip_space(tok.next());
        }
      }
      else {
        expand += tok_str;
      }
      if (tok == end) {
        break;
      }
      tok = skip_directive_newlines(tok.next());
    }

    /* Early out simple cases. */
    if (expand == "0") {
      return false;
    }
    if (expand == "1") {
      return true;
    }

    int value = 0;
    try {
      report_callback report = [](int, int, std::string, const char *) {};
      IntermediateForm parser(expand, report, ParserStage::MergeTokens, false);

      value = ExpressionParser(parser()[0]).eval();
    }
    catch (const std::exception &e) {
      std::cout << "\"" << parser.substr_range_inclusive_view(start, end) << "\" > \"" << expand
                << "\" ";
      std::cerr << "Error: " << e.what() << "\n";
    }

    return value != 0;
  }

  bool evaluate_condition(Token type, Token start, Token end)
  {
    StringRef type_str = str(type);
    if (type_str == "else") {
      return true;
    }
    if (type_str == "ifdef") {
      return defines.contains(str(start));
    }
    if (type_str == "ifndef") {
      return !defines.contains(str(start));
    }
    if (ELEM(type_str, "if", "elif")) {
      return evaluate_expression(start, end);
    }
    BLI_assert_unreachable();
    return true;
  }

  int process_conditional(const Token hash_tok, const Token dir_end)
  {
    /* If this is part of an already evaluated statement. */
    if (!jump_stack.is_empty() && hash_tok == jump_stack.last()) {
      jump_stack.pop_last();
      Token endif_hash = hash_tok;
      while (directive_identifier(endif_hash) != "endif") {
        endif_hash = find_next_matching_conditional_directive(endif_hash);
      }
      Token endif_end = end_of_directive(endif_hash);
      parser.replace(hash_tok, endif_end, new_lines(hash_tok, endif_end));
      return endif_end.index;
    }

    /* Evaluate condition. */
    const Token condition_type = skip_space(hash_tok.next());
    const Token condition_start = condition_type.next().next();
    const Token condition_end = dir_end;
    bool condition_result = evaluate_condition(condition_type, condition_start, condition_end);

    /* Find matching endif or else. */
    const Token next_hash = find_next_matching_conditional_directive(hash_tok);

    if (condition_result == false) {
      /* Erase the content and jump to next condition. */
      parser.replace(hash_tok, next_hash.prev(), new_lines(hash_tok, next_hash.prev()));
      return next_hash.prev().index;
    }
    /* If condition is true. */
    {
      /* If is followed by else statement. */
      StringRef next_dir_str = directive_identifier(next_hash);
      if (ELEM(next_dir_str, "elif", "else")) {
        /* Record a jump statement at the next #else statement to jump & erase to the #endif. */
        jump_stack.append(next_hash);
      }
      /* Erase condition and continue parsing content.
       * The #endif will just be erased later. */
      parser.replace(hash_tok, dir_end, new_lines(hash_tok, dir_end));
      return dir_end.index;
    }
  }

#ifndef NDEBUG
#  define CHECK(cond) \
    if (cond) { \
      return; /* TODO(fclem): Error. */ \
    }
#else
#  define CHECK(cond)
#endif

  void define_macro(Token hash_tok, Token dir_tok, Token dir_end)
  {
    Token macro_name = skip_space(dir_tok.next());
    CHECK(macro_name != Word);
    /* Store the name token of the declaration.
     * The actual parsing of the definition happens during expansion. */
    defines.add_overwrite(str(macro_name), macro_name);
    parser.replace(hash_tok, dir_end, new_lines(hash_tok, dir_end));
  }

  void undefine_macro(Token hash_tok, Token dir_tok, Token dir_end)
  {
    Token macro_name = skip_space(dir_tok.next());
    CHECK(macro_name != Word);
    defines.remove(str(macro_name));
    erase_single_line_directive(hash_tok, dir_end);
  }

  void erase_single_line_directive(Token hash_tok, Token dir_end)
  {
    /* Don't need new_lines in this case. */
    parser.replace(hash_tok, dir_end, "");
  }

  void process_directives(const TokenStream &data, int &cursor)
  {
    Token hash_tok = Token::from_position(&data, cursor);
    /* All directives must start with a hash token at the start of the line. */
    CHECK(!ELEM(hash_tok.prev(), Invalid /* Start of file. */, NewLine, Space));

    Token dir_tok = skip_space(hash_tok.next());

    CHECK(dir_tok != Word);

    Token dir_end = end_of_directive(dir_tok);
    StringRef dir_str = str(dir_tok);

    DirectiveType type = to_directive_type(dir_str);

    cursor = dir_end.index;

    switch (type) {
      case Define:
        define_macro(hash_tok, dir_tok, dir_end);
        break;
      case Undef:
        undefine_macro(hash_tok, dir_tok, dir_end);
        break;
      case If:
      case Ifdef:
      case Ifndef:
      case Elif:
      case Else:
        cursor = process_conditional(hash_tok, dir_end);
        break;
      case Endif:
      case Line:
        erase_single_line_directive(hash_tok, dir_end);
        break;
      case Other:
        break;
    }
  }

  void preprocess()
  {
    const TokenStream &data = parser.data_get();

    int cursor = 0;
    for (; cursor < data.token_types.size(); cursor++) {
      TokenType tok_type = TokenType(data.token_types[cursor]);
      if (tok_type == Word) {
        try_expand(parser, data, cursor);
      }
      else if (tok_type == Hash) {
        process_directives(data, cursor);
      }
    }
  }
};

std::string Shader::run_preprocessor(StringRef source)
{
  report_callback report = [](int, int, std::string, const char *) {};

  IntermediateForm parser(source, report, ParserStage::TokenizePreprocessor, false);

  Preprocessor processor{parser};
  processor.preprocess();

  return parser.result_get(true);
}

}  // namespace blender::gpu
