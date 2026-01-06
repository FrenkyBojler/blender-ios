/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "shader_tool/intermediate.hh"

#include "gpu_shader_private.hh"

namespace blender::gpu {

using namespace shader::parser;

/* Fast C (incomplete) preprocessor implementation.  */
struct Preprocessor {
  IntermediateForm &parser;

  Map<StringRef, Token> defines;
  Set<StringRef> visited_macros;
  /* Token cursor. */
  int cursor;

  static StringRef str(Token t)
  {
    /* Note: Whitespaces where not merged (because of TokenizePreprocessor), so using
     * str_view_with_whitespace will be faster.  */
    return t.str_view_with_whitespace();
  }

  static Token end_of_directive(Token define_tok)
  {
    Token tok = define_tok;

    while (tok != NewLine) {
      if (tok.next() == Invalid) {
        /* Error or end of file. */
        return tok;
      }
      tok = tok.next();
      if (tok == '\\' && tok.next() == '\n') {
        tok = tok.next().next();
      }
    }
    return tok.prev();
  }

  std::string expand_macro(Token expanded_tok, Token macro_name)
  {
    Token tok = macro_name.next();
    /* Skip spaces. */
    if (tok == Space) {
      tok = tok.next();
    }
    /* Empty definition. */
    if (tok == '\n') {
      return "";
    }

    StringRef macro_name_str = str(macro_name);
    /* Add to the set to avoid infinite recursion. */
    if (!visited_macros.add(macro_name_str)) {
      /* Recursion. Do not expand. Still replace by the original token. */
      return macro_name_str;
    }

    if (tok == '(') {
      /* This is a functional macro. */

      /* Parse parameters. */
      Token parameter_open = expanded_tok.next();
      if (parameter_open != '(') {
        /* Error, macro doesn't have parameters. */
        visited_macros.remove(macro_name_str);
        return macro_name_str;
      }

      /* Parse arguments. */
    }

    std::string expanded;
    expanded.reserve(256);

    while (tok != NewLine) {
      if (tok == ' ') {
        /* Replace multiple spaces by only one. Shrinks final codebase. */
        expanded += ' ';
      }
      else if (tok == Word) {
        Token macro_tok = Token::invalid();

        /* TODO(fclem): Functional macro args. */
        // macro_tok = arguments.lookup_default(str(tok), Token::invalid());

        if (macro_tok.is_invalid()) {
          macro_tok = defines.lookup_default(str(tok), Token::invalid());
          if (macro_tok.is_valid()) {
            expanded += expand_macro(tok, macro_tok);
          }
        }

        if (macro_tok.is_invalid()) {
          expanded += str(tok);
        }
      }
      else {
        expanded += str(tok);
      }

      tok = tok.next();
      if (tok == '#' && tok.next() == '#') {
        /* Token concat. */
        tok = tok.next().next();
      }
      if (tok == '\\' && tok.next() == '\n') {
        /* Preprocessor new line. Skip and continue. */
        tok = tok.next().next();
        /* Still insert a space to avoid merging tokens. */
        expanded += ' ';
      }
      else if (tok == Invalid) {
        /* Error. */
        break;
      }
    }

    visited_macros.remove(macro_name_str);
    return expanded;
  }

  static Token find_next_conditional_directive(Token hash_tok)
  {
    while (1) {
      hash_tok = hash_tok.find_next(Hash);
      if (hash_tok == Invalid) {
        return hash_tok;
      }
      Token dir_tok = hash_tok.next();
      /* Skip optional spaces after hash token. */
      if (dir_tok == Space) {
        dir_tok = dir_tok.next();
      }
      StringRef dir_str = str(dir_tok);
      if (ELEM(dir_str, "if", "ifdef", "ifndef", "else", "elif", "endif")) {
        return dir_tok;
      }
    }
    BLI_assert_unreachable();
    return Token::invalid();
  }

  // static void process_conditional(IntermediateForm &parser, Token hash_tok)
  // {
  //   /* Find matching endif or else. */
  //   int stack = 1;
  //   tok = tok.next();
  //   while (tok.is_valid()) {
  //     hash_tok = find_next_conditional_directive(hash_tok);

  //     if (hash_tok) {
  //       return;
  //     }

  //     if (tok == open) {
  //       stack++;
  //     }
  //     else if (tok == close) {
  //       stack--;
  //     }
  //     if (stack == 0) {
  //       return tok;
  //     }
  //     tok = tok.next();
  //   }
  //   return tok; /* Not found, return Invalid. */
  // }

  void process_directives(Token hash_tok)
  {
#ifndef NDEBUG
    Token prev = hash_tok.prev();
    if (!ELEM(prev, Invalid /* Start of file. */, NewLine, Space)) {
      /* All directives must start with a hash token at the start of the line. */
      return; /* TODO(fclem): Error. */
    }
#endif

    Token dir_tok = hash_tok.next();
    /* Skip optional spaces after hash token. */
    if (dir_tok == Space) {
      dir_tok = dir_tok.next();
    }

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
      parser.erase(hash_tok, dir_end);
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
      parser.erase(hash_tok, dir_end);
    }
    else if (dir_str == "ifndef") {
      /* Macro undefine. */
      Token space = dir_tok.next();
      if (space != Space) {
        return; /* TODO(fclem): Error. */
      }
      Token macro_name = space.next();
      if (macro_name != Word) {
        return; /* TODO(fclem): Error. */
      }
      if (defines.contains(str(macro_name))) {
        //   cursor = process_conditional(parser, hash_tok);
      }
    }
    else if (dir_str == "ifdef") {
      // if (defines.find(t.next().str_view()) == defines.end()) {
      //   cursor = process_conditional(parser, t.prev());
      // }
    }
    else if (dir_str == "line") {
      parser.erase(hash_tok, dir_end);
    }
    cursor = dir_end.index;
  }

  void preprocess()
  {
    int macro_replacement_count = 0;
    const TokenStream &data = parser.data_get();

    cursor = 0;
    for (; cursor < data.token_types.size(); cursor++) {
      TokenType tok_type = TokenType(data.token_types[cursor]);
      if (tok_type == Word) {
        Token tok = Token::from_position(&data, cursor);
        Token macro_tok = defines.lookup_default(str(tok), Token::invalid());
        if (macro_tok.is_valid()) {
          parser.replace(tok, expand_macro(tok, macro_tok));
          macro_replacement_count++;
        }
      }
      else if (tok_type == Hash) {
        process_directives(Token::from_position(&data, cursor));
      }
    }
    std::cout << "Macro hit " << macro_replacement_count << std::endl;
    std::cout << "Macro def " << defines.size() << std::endl;
  }
};

std::string Shader::run_preprocessor(StringRef source)
{
  report_callback report = [](int, int, std::string, const char *) {};

  IntermediateForm parser(source, report, ParserStage::TokenizePreprocessor);

  Preprocessor processor{parser};
  processor.preprocess();

  parser.print_stats();

  return parser.result_get();
}

}  // namespace blender::gpu
