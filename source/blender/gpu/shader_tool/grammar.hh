/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 *
 */

#include "token.hh"
#include "token_stream.hh"

/* TODO:
 * - Scope output
 * - variable decl
 * - Preprocessor
 */

namespace blender::gpu::shader::parser {

struct GrammarParser {
  Token curr;

  GrammarParser(Token tok) : curr(tok) {}

  std::string error_str_;

  bool error(std::string str)
  {
    error_str_ = "filename:";
    error_str_ += std::to_string(curr.line_number()) + ':';
    error_str_ += std::to_string(curr.char_number()) + ':';
    error_str_ += " error: ";
    error_str_ += str;
    error_str_ += '\n';
    error_str_ += curr.line_str();
    error_str_ += '\n';
    error_str_ += std::string(curr.char_number(), ' ') + '^';
    error_str_ += '\n';
    return false;
  }

  bool lookahead(TokenType expected) const
  {
    return curr == expected;
  }

  bool lookahead(char c) const
  {
    return lookahead(TokenType(c));
  }

#define dbg(...) std::cout << std::string(depth * 2, ' ') << __VA_ARGS__ << std::endl
#define prt(...)  // std::cout << std::string(depth * 2, ' ') << __VA_ARGS__ << std::endl

  bool tk(TokenType expected)
  {
    bool result = curr == expected;
    if (!result) {
      return error(std::string("Syntax Error: Expected token type ") + char(expected) +
                   " but got " + char(curr.type()));
    }
    prt(char(expected));
    curr = curr.next();
    return result;
  }

  bool tk(char c)
  {
    return tk(TokenType(c));
  }
  bool tk(char c1, char c2)
  {
    return tk(TokenType(c1)) && tk(TokenType(c2));
  }
  bool tk(std::string_view view)
  {
    for (char c : view) {
      if (!tk(TokenType(c))) {
        return false;
      }
    }
    return true;
  }

  /* Saved index to rollback in case of match failure. */
  std::vector<Token> saved_;

  bool push()
  {
    saved_.push_back(curr);
    return false;
  }

  bool pop()
  {
    curr = saved_.back();
    saved_.pop_back();
    return false;
  }

  bool drop()
  {
    saved_.pop_back();
    return true;
  }

  /* Evaluate but roll back on failure.
   * Equivalent to * in BNF. */
#define try(a) (push() || ((a) && drop()) || pop())
  /* Optional statement. Will always evaluate to true.
   * Equivalent to ? in BNF. */
#define opt(a) (push() || ((a) && drop()) || !pop())

  /* <translation_unit> ::= <external_declaration_list> */
  void translation_unit()
  {
    /* Skip first whitespace token if it exists. */
    if (lookahead(NewLine) || lookahead(Space)) {
      curr = curr.next();
    }

    external_declaration_list();

    if (!lookahead(EndOfFile)) {
      std::cerr << error_str_ << std::endl;
      assert(lookahead(EndOfFile));
    }
  }

  int depth = 0;

#define rule(name, ...) \
  bool name() \
  { \
    ++depth; \
    dbg(#name << " " << curr.str() << " enter"); \
    bool value = __VA_ARGS__; \
    dbg(#name << " " << curr.str() << " exit " << value); \
    if (value) { \
      prt(#name); \
    } \
    --depth; \
    return value; \
  }

  /* Equivalent to * in BNF. */
#define list(name, ...) \
  bool name() \
  { \
    ++depth; \
    dbg(#name << " " << curr.str() << " enter"); \
    while (__VA_ARGS__) { \
    } \
    dbg(#name << " " << curr.str() << " exit"); \
    --depth; \
    return true; \
  }

  rule(unqualified_id, /**/
       tk(Word));

  rule(identifier, /**/
       tk(Word));

  rule(global_namespace_specifier, /**/
       tk("::"));

  rule(pointer, /**/
       tk('&'));

  /* TODO template */
  rule(qualified_id, /**/
       opt(global_namespace_specifier()) && opt(nested_name_specifier()) && unqualified_id());

  rule(namespace_id, /**/
       opt(nested_name_specifier()) && unqualified_id());

  rule(nested_name_specifier, /**/
       unqualified_id() && tk("::") && opt(nested_name_specifier()));

  rule(type_specifier, /* Should be int/float and all builtin types + user defined types. */
       qualified_id());

  rule(type_qualifier, /**/
       tk(Const));

  rule(external_declaration, /**/
       try(namespace_scope()) || try(declaration()) /* || try(function_definition()) */);

  list(external_declaration_list, /**/
       external_declaration());

  rule(namespace_scope, /**/
       tk(Namespace) && namespace_id() && tk('{') && external_declaration_list() && tk('}'));

  rule(declaration, /**/
       declaration_specifier() && init_declarator_list() && opt(init_declarator()) && tk(';'));

  rule(declaration_specifier, /**/
       opt(type_qualifier()) && type_specifier());

  rule(init_declarator, /**/
       declarator() && opt(tk('=') && initializer()));

  rule(declarator, /**/
       opt(pointer()) && identifier() &&
           opt(try(array_declarator()) || try(function_args_declarator())));

  rule(array_declarator, /**/
       tk('[') && opt(constant_expr()) && tk(']') && opt(array_declarator()));

  rule(function_args_declarator, /**/
       tk('(') && function_arg_list() && opt(declarator()) && tk(')'));

  rule(function_arg, /**/
       declarator());

  list(function_arg_list, /**/
       try(declarator() && tk(',')));

  list(init_declarator_list, /**/
       try(init_declarator() && tk(',')));

  rule(initializer, /**/
       try(assign_expr()) || try(tk('{') && initializer_list() && opt(initializer()) && tk('}')));

  list(initializer_list, /**/
       try(initializer() && tk(',')));

  rule(assign_expr, /* TODO */
       try(tk(Number)) || try(tk(Word)));

  rule(constant_expr, /* TODO */
       try(tk(Number)) || try(tk(Word)));

#undef try
#undef opt
#undef rule
#undef list
};
}  // namespace blender::gpu::shader::parser
