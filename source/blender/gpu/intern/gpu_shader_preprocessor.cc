/* SPDX-FileCopyrightText: 2005 Blender Authors
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
  Map<StringRef, TokenRange> macro_parameters;
  /* Token cursor. */
  int cursor;

  static StringRef str(Token t)
  {
    /* Note: Whitespaces where not merged (because of TokenizePreprocessor), so using
     * str_view_with_whitespace will be faster.  */
    return t.str_view_with_whitespace();
  }

  static StringRef str(TokenRange range)
  {
    /* Note: Whitespaces where not merged (because of TokenizePreprocessor), so using
     * str_view_with_whitespace will be faster.  */
    StringRef start = range.start.str_view_with_whitespace();
    StringRef end = range.end.str_view_with_whitespace();
    return {start.data(), end.data() - start.data() + end.size()};
  }

  std::string new_lines(Token start, Token end)
  {
    std::string_view dir_str = parser.substr_range_inclusive_view(start, end);
    int line_count = std::count(dir_str.begin(), dir_str.end(), '\n');
    return std::string(line_count, '\n');
  }

  static Token end_of_directive(Token dir_tok)
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

  static Token skip_whitespace(Token tok)
  {
    while (tok == Space) {
      tok = tok.next();
    }
    return tok;
  }

  static Token skip_whitespace_backward(Token tok)
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

  static Token get_end_of_parameter(Token tok, bool skip_to_end = false)
  {
    /* Avoid matching coma inside parameter function calls. */
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

    tok = skip_whitespace(tok);

    /* Empty definition. */
    if (tok == '\n') {
      return {"", end_of_expansion};
    }

    StringRef macro_name_str = str(macro_name);
    /* Add to the set to avoid infinite recursion. */
    if (!visited_macros.add(macro_name_str)) {
      /* Recursion. Do not expand. Still replace by the original token. */
      return {macro_name_str, end_of_expansion};
    }

    if (is_function) {
      /* This is a functional macro. */

      Token param = expanded_tok.next();
      if (param != '(') {
        /* Error, macro doesn't have parameters. */
        visited_macros.remove(macro_name_str);
        return {macro_name_str, end_of_expansion};
      }

      /* Parse parameters & arguments. */
      macro_parameters.clear_and_keep_capacity();
      while (tok != ')') {
        /* Continue to the next name. */
        tok = skip_whitespace(tok.next());
        if (tok == ')') {
          /* Function with no arguments. */
          param = get_end_of_parameter(param);
          if (param != ')') {
            /* Error. There are parameters inside the function call. */
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

        macro_parameters.add(
            argument_name,
            {skip_whitespace(param_start.next()), skip_whitespace_backward(param_end.prev())});

        /* Continue to the next separator. */
        tok = skip_whitespace(tok.next());
        param = param_end;

        if (tok == Invalid) {
          break;
        }
      }
      /* Skip closing parenthesis. */
      tok = skip_whitespace(tok.next());
      /* Make sure to replace the whole call. */
      end_of_expansion = param;
    }

    std::string expanded;
    expanded.reserve(256);

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

      if (tok == ' ') {
        /* Replace multiple spaces by only one. Shrinks final codebase. */
        expanded += ' ';
      }
      else if (tok == Word) {
        bool replaced = false;

        if (is_function) {
          /* Lookup macro arguments. */
          TokenRange *macro_value_ptr = macro_parameters.lookup_ptr(str(tok));
          if (macro_value_ptr) {
            TokenRange &macro_value = *macro_value_ptr;
            /* Expand argument. */
            expanded += str(macro_value);
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

      tok = tok.next();
    }

    if (!expanded.empty()) {
      report_callback report = [](int, int, std::string, const char *) {};
      IntermediateForm parser(expanded, report, ParserStage::TokenizePreprocessor);

      const TokenStream &data = parser.data_get();

      for (int cursor = 0; cursor < data.token_types.size(); cursor++) {
        TokenType tok_type = TokenType(data.token_types[cursor]);
        if (tok_type == Word) {
          Token tok = Token::from_position(&data, cursor);
          Token macro_tok = defines.lookup_default(str(tok), Token::invalid());
          if (macro_tok.is_valid()) {
            auto [replacement, end] = expand_macro(tok, macro_tok);
            parser.replace(tok, end, replacement);
            cursor = end.index;
          }
        }
      }
      expanded = parser.result_get();
    }

    visited_macros.remove(macro_name_str);
    return {expanded, end_of_expansion};
  }

  /* Returns the hash token. */
  static Token find_next_conditional_directive(Token hash_tok)
  {
    while (true) {
      hash_tok = hash_tok.find_next(Hash);
      if (hash_tok == Invalid) {
        return hash_tok;
      }
      Token dir_tok = skip_whitespace(hash_tok.next());
      StringRef dir_str = str(dir_tok);
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
      StringRef dir_str = str(skip_whitespace(tok.next()));
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
        tok = tok.next();
        const bool is_function = (tok == '(');
        /* Token to search. */
        tok = tok.next();
        expand += (defines.contains(str(tok)) ? "1" : "0");
        if (is_function) {
          /* End parenthesis. */
          tok = tok.next();
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

    std::cout << "\"" << parser.substr_range_inclusive_view(start, end) << "\" > \"" << expand
              << "\" ";

    int value = 0;
    try {
      report_callback report = [](int, int, std::string, const char *) {};
      IntermediateForm parser(expand, report, ParserStage::MergeTokens);

      value = ExpressionParser(parser()[0]).eval();
      std::cout << "Result = " << value << "\n";
    }
    catch (const std::exception &e) {
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
      return defines.contains(str(end));
    }
    if (type_str == "ifndef") {
      return !defines.contains(str(end));
    }
    if (ELEM(type_str, "if", "elif")) {
      return evaluate_expression(start, end);
    }
    BLI_assert_unreachable();
    return true;
  }

  int process_conditional(const Token hash_tok, const Token dir_end)
  {
    /* Evaluate condition. */
    const Token condition_type = skip_whitespace(hash_tok.next());
    const Token condition_start = condition_type.next().next();
    const Token condition_end = dir_end;
    bool condition_result = evaluate_condition(condition_type, condition_start, condition_end);

    /* Find matching endif or else. */
    const Token next_dir = find_next_matching_conditional_directive(hash_tok);

    if (condition_result == false) {
      /* Erase the content and jump to next condition. */
      parser.replace(hash_tok, next_dir.prev(), new_lines(hash_tok, next_dir.prev()));
      return next_dir.prev().index;
    }
    /* If condition is true. */
    {
      /* If is followed by else statement. */
      StringRef next_dir_str = str(next_dir);
      if (ELEM(next_dir_str, "elif", "else")) {
        /* Record a jump statement at the next #else statement to jump & erase to the #endif. */
        jump_stack.append(next_dir);
      }
      /* Erase condition and continue parsing content.
       * The #endif will just be erased later. */
      parser.replace(hash_tok, dir_end, new_lines(hash_tok, dir_end));
      return dir_end.index;
    }
  }

  void process_directives(Token hash_tok)
  {
#ifndef NDEBUG
    Token prev = hash_tok.prev();
    if (!ELEM(prev, Invalid /* Start of file. */, NewLine, Space)) {
      /* All directives must start with a hash token at the start of the line. */
      return; /* TODO(fclem): Error. */
    }
#endif

    Token dir_tok = skip_whitespace(hash_tok.next());

    if (dir_tok != Word) {
      return; /* TODO(fclem): Error. */
    }

    Token dir_end = end_of_directive(dir_tok);
    StringRef dir_str = str(dir_tok);

    if (dir_str == "define") {
      /* Macro definition. */
      Token space = dir_tok.next();
      if (space != Space) {
        return; /* TODO(fclem): Error. */
      }
      Token macro_name = space.next();
      if (macro_name != Word) {
        return; /* TODO(fclem): Error. */
      }
      defines.add_overwrite(str(macro_name), macro_name);
      parser.replace(hash_tok, dir_end, new_lines(hash_tok, dir_end));
    }
    else if (dir_str == "undef") {
      /* Macro undefine. */
      Token space = dir_tok.next();
      if (space != Space) {
        return; /* TODO(fclem): Error. */
      }
      Token macro_name = space.next();
      if (macro_name != Word) {
        return; /* TODO(fclem): Error. */
      }
      defines.remove(str(macro_name));
      parser.replace(hash_tok, dir_end, "");
    }
    else if (ELEM(dir_str, "if", "ifdef", "ifndef", "elif", "else")) {
      /* Conditional. */

      /* If this is part of an already evaluated statement. */
      if (!jump_stack.is_empty() && dir_tok == jump_stack.last()) {
        jump_stack.pop_last();
        Token endif_hash = hash_tok;
        while (str(endif_hash) != "endif") {
          endif_hash = find_next_matching_conditional_directive(endif_hash);
        }
        Token endif_end = end_of_directive(endif_hash);
        cursor = endif_end.index;
        parser.replace(hash_tok, endif_end, new_lines(hash_tok, endif_end));
        return;
      }

      cursor = process_conditional(hash_tok, dir_end);
      return;
    }
    else if (dir_str == "line") {
      parser.replace(hash_tok, dir_end, "");
    }
    else if (dir_str == "endif") {
      parser.replace(hash_tok, dir_end, "");
    }
    cursor = dir_end.index;
  }

  void preprocess()
  {
    const TokenStream &data = parser.data_get();

    cursor = 0;
    for (; cursor < data.token_types.size(); cursor++) {
      TokenType tok_type = TokenType(data.token_types[cursor]);
      if (tok_type == Word) {
        Token tok = Token::from_position(&data, cursor);
        Token macro_tok = defines.lookup_default(str(tok), Token::invalid());
        if (macro_tok.is_valid()) {
          auto [replacement, end] = expand_macro(tok, macro_tok);
          parser.replace(tok, end, replacement);
          cursor = end.index;
        }
      }
      else if (tok_type == Hash) {
        process_directives(Token::from_position(&data, cursor));
      }
    }
    // std::cout << "Macro def " << defines.size() << std::endl;
  }
};

std::string Shader::run_preprocessor(StringRef source)
{
  report_callback report = [](int, int, std::string, const char *) {};

  IntermediateForm parser(source, report, ParserStage::TokenizePreprocessor);

  Preprocessor processor{parser};
  processor.preprocess();

  return parser.result_get();
}

}  // namespace blender::gpu
