/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 *
 */

#include "scope.hh"
#include "token.hh"
#include "token_stream.hh"

/* TODO:
 * - Scope output
 * - variable decl
 * - Preprocessor
 */

namespace blender::gpu::shader::parser {

struct ErrorLog {
  std::vector<std::pair<Token, std::string>> errors;

  void error(Token tok, const std::string &str)
  {
    std::string err;
    //     err = "filename:";
    //     err += std::to_string(tok.line_number()) + ':';
    //     err += std::to_string(tok.char_number()) + ':';
    err += " error: ";
    err += str;
    //     err += '\n';
    //     err += tok.line_str();
    //     err += '\n';
    //     err += std::string(tok.char_number(), ' ') + '^';
    //     err += '\n';
    //     std::cerr << err << std::endl;
    errors.emplace_back(tok, err);
  }
};

struct Tree {
  using Node = Scope;
  ParserBase &parser;
  Tree(ParserBase &parser) : parser(parser) {}

  Node curr = Node(parser);

  void open_scope(Token tok, ScopeType type)
  {
    int index = parser.scope_types.size();
    parser.scope_types.emplace_back(type);
    parser.scope_ranges.emplace_back(tok.index_, 1);

    ScopeLinks &links = parser.scope_links.emplace_back();
    links.parent_ = curr.index_;
    if (links.parent_ != -1) {
      ScopeLinks &parent_links = parser.scope_links[links.parent_];
      if (parent_links.child_first_ == -1) {
        parent_links.child_first_ = index;
      }
      links.prev_ = parent_links.child_last_;
      parent_links.child_last_ = index;
    }
    if (links.prev_ != -1) {
      parser.scope_links[links.prev_].next_ = index;
    }

    curr = Node(parser, index);
  }

  void close_scope(Token tok, ScopeType type)
  {
    if (curr.type() == type) {
      IndexRange &range = parser.scope_ranges[curr.index_];
      range.size = tok.index_ - range.start + 1;
      curr = curr.parent();
    }
  }
};

struct ScopeParser {
  Token curr;

  ParserBase &parser;
  Tree tree;
  ErrorLog log;

  ScopeParser(ParserBase &parser, Token tok) : curr(tok), parser(parser), tree(parser) {}

  TokenType peek() const
  {
    return curr.type();
  }

  void error(const std::string &str)
  {
    log.error(curr, str);
    next();
  }

  void match(char expected)
  {
    if (curr != TokenType(expected)) {
      error(std::string("Syntax Error: Expected token type ") + expected + " but got " +
            char(curr.type()));
    }
    curr = curr.next();
  }

  void match(char expected, char expected2)
  {
    if (curr != TokenType(expected) && curr != TokenType(expected2)) {
      error(std::string("Syntax Error: Expected token type ") + expected + " or " + expected +
            " but got " + char(curr.type()));
    }
    curr = curr.next();
  }

  /* Only go to next token if matching an optional token. */
  bool match_if(char expected)
  {
    if (curr == TokenType(expected)) {
      curr = curr.next();
      return true;
    }
    return false;
  }

  Token next()
  {
    return curr = curr.next();
  }

  void translation_unit()
  {
    tree.open_scope(curr, ScopeType::Global);
    /* Skip first whitespace token if it exists. */
    if (peek() == NewLine || peek() == Space) {
      curr = curr.next();
    }
    external_declaration();
    tree.close_scope(parser.back(), ScopeType::Global);
    match(EndOfFile);
  }

  void external_declaration()
  {
    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case SquareOpen:
          attribute_or_subscript();
          break;
        case Namespace:
          namespace_declaration();
          break;
        case Class:
        case Struct:
          struct_declaration();
          break;
        case Enum:
          enum_declaration();
          break;
        case ParOpen:
          function_definition();
          break;
        case TemplateOpen:
          template_argument_list();
          break;
        case Template:
          template_definition();
          break;
        case Using:
          next();
          match_if(Namespace);
          qualified_id();
          break;
        case Assign:
          assignment();
          break;
        case BracketOpen:
          /* For C++/HLSL compatibility. */
          local_scope(ScopeType::Local);
          break;
        case Const:
        case Colon:
        case Constexpr:
        case SemiColon:
        case Inline:
        case Static:   /* For C++ compatibility. */
        case NotEqual: /* For MSL matrix operators. */
        case Minus:    /* For MSL matrix operators. */
        case Word:
          next();
          break;
        case EndOfFile:
        case BracketClose:
          return;
        default:
          error("Unexpected token: Expecting declaration");
          break;
      }
    }
  }

  /* Example: `struct [[a]] A {}`. */
  void struct_declaration()
  {
    match(Struct, Class);
    /* Optional attributes. */
    if (peek() == '[') {
      attribute();
    }

    if (peek() == '{') {
      /* Nameless struct */
    }
    else {
      /* Note we allow `struct A::B` syntax because it is used during namespace lowering. */
      qualified_id();
      if (peek() == ';') {
        /* Allowed because of shared C++ files which have C++ code not yet rejected. */
        //  error("Forward declaration of classes is not supported");
        return;
      }
      if (peek() == Word) {
        /* Struct keyword usage in variable declaration. */
        /* Supported because of explicit host shared struct members and C++ shared code. */
        next();
        return;
      }
    }
    /* For specialization. */
    if (peek() == lexit::TemplateOpen) {
      template_argument_list();
    }
    tree.open_scope(curr, ScopeType::Struct);
    match('{');
    member_declaration();
    tree.close_scope(curr, ScopeType::Struct);
    match('}');
  }

  void member_declaration()
  {
    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case SquareOpen:
          attribute_or_subscript();
          break;
        case Private:
        case Public:
          next();
          match(':');
          break;
        case Class:
        case Struct:
          // error("Nested class declaration is not supported");
          // return;
          /* Supported because of explicit host shared struct members and C++ shared code. */
          struct_declaration();
          break;
        case Enum:
          // error("Nested enum declaration not supported");
          // return;
          /* Supported because of explicit host shared struct members. */
          next();
          break;
        case Union:
          union_declaration();
          break;
        case ParOpen:
          function_definition();
          break;
        case TemplateOpen:
          template_argument_list();
          break;
        case Template:
          template_definition();
          break;
        case BracketClose:
          return;
        case Assign:
          assignment();
          break;
        case Using:
        case Const:
        case Constexpr:
        case Static:
        case SemiColon:
        case Colon:
        case Ampersand: /* For references. */
        case Inline:    /* For MSL / C++. */
        case Number:    /* For C++ bitflags. */
        case Star:      /* For C++ pointers. */
        case Comma:     /* For C++ constructor. */
        case Equal:     /* For C++ operator. */
        case Word:
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  /* Example: `enum [[a]] A : a {}`. */
  void enum_declaration()
  {
    match(Enum);
    /* Optional attributes. */
    if (peek() == '[') {
      attribute();
    }
    /* Note we allow `struct A::B` syntax because it is used during namespace lowering. */
    match(Word);
    if (match_if(':')) {
      /* Underlying type. */
      match(Word);
    }

    tree.open_scope(curr, ScopeType::Local);
    match('{');
    enum_values();
    tree.close_scope(curr, ScopeType::Local);
    match('}');
  }

  void enum_values()
  {
    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case ParOpen:
          local_parenthesis();
          break;
        case BracketClose:
          return;
        case Assign:
          assignment();
          break;
        case Comma:
        case Word:
        case Number:
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  /* Example: `union {}`. */
  void union_declaration()
  {
    match(Union);
    tree.open_scope(curr, ScopeType::Local);
    match('{');
    member_declaration();
    tree.close_scope(curr, ScopeType::Local);
    match('}');
  }

  void template_definition()
  {
    match(Template);

    if (peek() == Word) {
      /* Template instantiation. */
      return;
    }
    template_argument_list();
  }

  void template_explicit_call()
  {
    if (curr.prev() != '.') {
      /* Expected a method call. */
      match(Word);
    }
    else {
      match(Template);
    }
  }

  void template_argument_list()
  {
    tree.open_scope(curr, ScopeType::Template);
    match(TemplateOpen);

    bool in_argument = false;
    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case TemplateOpen:
          template_argument_list();
          break;
        case TemplateClose:
          if (in_argument) {
            in_argument = false;
            tree.close_scope(curr.prev(), ScopeType::TemplateArg);
          }
          tree.close_scope(curr, ScopeType::Template);
          match(TemplateClose);
          return;
        case Comma:
          if (in_argument) {
            in_argument = false;
            tree.close_scope(curr.prev(), ScopeType::TemplateArg);
          }
          next();
          break;
        case Assign:
          if (!in_argument) {
            /* Expecting at least a type before an assignment. */
            match(Word);
          }
          /* For MSL/C++ compatibility. */
          assignment();
          break;
        case Star:   /* For C++ shared headers. */
        case Class:  /* Used during union processing. */
        case Struct: /* Used during union processing. */
        case Colon:  /* Could be removed if using qualified_id(). */
        case LThan:
        case Plus:
        case Minus:
        case Divide:
        case Modulo:
        case GThan:
        case LEqual:
        case GEqual:
        case Equal:
        case Const:
        case Word:
        case Number:
          if (!in_argument) {
            tree.open_scope(curr, ScopeType::TemplateArg);
            in_argument = true;
          }
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  /* Example: `A::T<t> A::B<T>(){}`. */
  void function_definition()
  {
    function_argument_list();
    match_if(Const);
    if (match_if(';')) {
      /* Template instantiation or forward declaration. */
      return;
    }
    if (peek() == '{') {
      local_scope(ScopeType::Function);
      return;
    }
    /* Function call.
     *  Could eventually become an error but is currently used by create info macros. */
  }

  void assignment()
  {
    tree.open_scope(curr, ScopeType::Assignment);
    match('=');

    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case Template:
          template_explicit_call();
          break;
        case TemplateOpen:
          template_argument_list();
          break;
        case ParOpen:
          function_call_or_local_parenthesis();
          break;
        case SquareOpen:
          subscript();
          break;
        case BracketOpen:
          local_scope(ScopeType::Local);
          break;
        case Assign:
          tree.close_scope(curr.prev(), ScopeType::Assignment);
          assignment();
          return;
        case ParClose:
        case BracketClose:
        case TemplateClose:
        case Comma:
        case SemiColon:
        case Equal:
          tree.close_scope(curr.prev(), ScopeType::Assignment);
          return;
        case This:
        case Minus:
        case Plus:
        case Dot:
        case Multiply:
        case Colon:
        case Question:
        case Ampersand:
        case Word:
        case Number:
        case Divide:
        case LogicalAnd:
        case LogicalOr:
        case GThan:
        case LThan:
        case GEqual:
        case LEqual:
        case Not:
        case NotEqual:
        case Modulo:
        case BitwiseNot:
        case Or:
        case Xor:
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  void local_scope(ScopeType type)
  {
    tree.open_scope(curr, type);
    match('{');

    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case Template:
          template_explicit_call();
          break;
        case TemplateOpen:
          template_argument_list();
          break;
        case BracketOpen:
          local_scope(ScopeType::Local);
          break;
        case BracketClose:
          tree.close_scope(curr, type);
          match('}');
          return;
        case ParOpen:
          function_call_or_local_parenthesis();
          break;
        case SquareOpen:
          attribute_or_subscript();
          break;
        case Assign:
          assignment();
          break;
        case For:
          for_loop();
          break;
        case While:
          while_loop();
          break;
        case Switch:
          switch_statement();
          break;
        case If:
          next();
          condition(1, ScopeType::Local);
          break;
        case Else:
          next();
          if (peek() == If) {
            break;
          }
          local_scope(ScopeType::Local);
          break;
        case Using:
        case This:
        case Case: /* For switch cases. */
        case Comma:
        case Break:
        case Const:
        case Constexpr:
        case Continue:
        case Return:
        case SemiColon:
        case Minus:
        case Equal:
        case Plus:
        case Dot:
        case Colon:
        case Question:
        case Ampersand:
        case Word:
        case Star:
        case Divide:
        case LogicalAnd:
        case LogicalOr:
        case GThan:
        case LThan:
        case GEqual:
        case LEqual:
        case Not:
        case NotEqual:
        case BitwiseNot:
        case Or:
        case Xor:
        case Increment:
        case Decrement:
        case Number:
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  void switch_statement()
  {
    match(Switch);
    condition(1, ScopeType::SwitchArg);
    local_scope(ScopeType::SwitchBody);
  }

  void for_loop()
  {
    match(For);
    condition(3, ScopeType::LoopArgs);
    local_scope(ScopeType::LoopBody);
  }

  void while_loop()
  {
    match(While);
    condition(1, ScopeType::LoopArgs);
    local_scope(ScopeType::LoopBody);
  }

  void condition(int arg_needed, ScopeType type)
  {
    tree.open_scope(curr, type);
    match('(');

    int arg_count = 0;
    bool in_argument = false;
    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case ParOpen:
          if (!in_argument) {
            if (type == ScopeType::LoopArgs) {
              tree.open_scope(curr, ScopeType::LoopArg);
            }
            in_argument = true;
          }
          function_call_or_local_parenthesis();
          break;
        case ParClose:
          if (in_argument) {
            in_argument = false;
            ++arg_count;
            if (type == ScopeType::LoopArgs) {
              tree.close_scope(curr.prev(), ScopeType::LoopArg);
            }
          }
          tree.close_scope(curr, type);
          if (arg_count < arg_needed) {
            /* Error about missing semicolon. */
            error("Missing loop or conditional statement");
            return;
          }
          match(')');
          /* Optional attribute. */
          if (peek() == '[') {
            attribute();
          }
          return;
        case SemiColon:
          ++arg_count;
          if (in_argument && type == ScopeType::LoopArgs) {
            in_argument = false;
            tree.close_scope(curr.prev(), ScopeType::LoopArg);
          }
          next();
          break;
        case SquareOpen:
          subscript();
          break;
        case This:
        case Comma:
        case Colon:
        case Dot:
        case Assign:
        case Equal:
        case Increment:
        case Decrement:
        case LEqual:
        case GEqual:
        case NotEqual:
        case Not:
        case Word:
        case Multiply:
        case And:
        case Or:
        case Xor:
        case GThan:
        case LThan:
        case BitwiseNot:
        case Minus:
        case Plus:
        case Modulo:
        case Divide:
        case LogicalAnd:
        case LogicalOr:
        case Number:
          if (!in_argument) {
            if (type == ScopeType::LoopArgs) {
              tree.open_scope(curr, ScopeType::LoopArg);
            }
            in_argument = true;
          }
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  void function_argument_list()
  {
    tree.open_scope(curr, ScopeType::FunctionArgs);
    match('(');

    bool in_argument = false;
    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case ParOpen:
          if (!in_argument) {
            /* This could be enabled once we get rid of all global level macros. */
            // /* Expecting at least a type before a function call. */
            // match(Word);
            tree.open_scope(curr, ScopeType::FunctionArg);
            in_argument = true;
          }
          function_call_or_local_parenthesis();
          break;
        case BracketOpen:
          if (!in_argument) {
            /* Expecting at least a type before a function call. */
            match(Word);
          }
          /* For initializer list constructor of parameters. */
          local_scope(ScopeType::Local);
          break;
        case TemplateOpen:
          template_argument_list();
          break;
        case ParClose:
          if (in_argument) {
            in_argument = false;
            tree.close_scope(curr.prev(), ScopeType::FunctionArg);
          }
          tree.close_scope(curr, ScopeType::FunctionArgs);
          match(')');
          return;
        case Comma:
          if (in_argument) {
            in_argument = false;
            tree.close_scope(curr.prev(), ScopeType::FunctionArg);
          }
          next();
          break;
        case SquareOpen:
          if (!in_argument) {
            tree.open_scope(curr, ScopeType::FunctionArg);
            in_argument = true;
          }
          attribute_or_subscript();
          break;
        case Assign:
          if (!in_argument) {
            /* Expecting at least a type before an assignment. */
            match(Word);
          }
          assignment();
          break;
        case String:     /* Needed for legacy create info. */
        case Or:         /* Needed for legacy create info. */
        case Equal:      /* Needed for some macros. */
        case LThan:      /* Needed for some macros. */
        case GThan:      /* Needed for some macros. */
        case LogicalOr:  /* Needed for some macros. */
        case LogicalAnd: /* Needed for some macros. */
        case Dot:        /* Needed for some macros. */
        case Star:       /* Needed for pointers in shared files. */
        case Word:
        case Number:
        case Minus: /* For C++ constructors.  */
        case Plus:  /* For C++ constructors.  */
        case Const:
        case Constexpr:
        case Ampersand:
        case Colon:
          if (!in_argument) {
            tree.open_scope(curr, ScopeType::FunctionArg);
            in_argument = true;
          }
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  void function_call_or_local_parenthesis()
  {
    TokenType prev = curr.prev().type();
    if (prev == Word || prev == lexit::TemplateClose) {
      function_call();
    }
    else {
      local_parenthesis();
    }
  }

  void local_parenthesis()
  {
    tree.open_scope(curr, ScopeType::Local);
    match('(');

    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case ParOpen:
          function_call_or_local_parenthesis();
          break;
        case ParClose:
          tree.close_scope(curr, ScopeType::Local);
          match(')');
          return;
        case SquareOpen:
          subscript();
          break;
        case This:
        case Equal:
        case Comma:
        case Minus:
        case Plus:
        case Number:
        case Word:
        case Dot:
        case Colon:
        case Question:
        case Ampersand:
        case Star:
        case Divide:
        case LogicalAnd:
        case LogicalOr:
        case Or:
        case Xor:
        case GThan:
        case LThan:
        case GEqual:
        case LEqual:
        case Not:
        case NotEqual:
        case BitwiseNot:
        case Assign: /* Because LEqual and GEqual might not be parsed. */
        case Modulo:
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  void function_call()
  {
    tree.open_scope(curr, ScopeType::FunctionCall);
    match('(');

    bool in_argument = false;
    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case ParOpen:
          if (!in_argument) {
            tree.open_scope(curr, ScopeType::FunctionParam);
            in_argument = true;
          }
          function_call_or_local_parenthesis();
          break;
        case ParClose:
          if (in_argument) {
            in_argument = false;
            tree.close_scope(curr.prev(), ScopeType::FunctionParam);
          }
          tree.close_scope(curr, ScopeType::FunctionCall);
          match(')');
          return;
        case Template:
          template_explicit_call();
          break;
        case TemplateOpen:
          template_argument_list();
          break;
        case BracketOpen:
          local_scope(ScopeType::Local);
          break;
        case Comma:
          if (in_argument) {
            in_argument = false;
            tree.close_scope(curr.prev(), ScopeType::FunctionParam);
          }
          next();
          break;
        case SquareOpen:
          subscript();
          break;
        case String:
        case This:
        case Minus:
        case Plus:
        case Number:
        case Word:
        case Dot:
        case Colon:
        case Question:
        case Ampersand:
        case Star:
        case Divide:
        case LogicalAnd:
        case Or:
        case Xor:
        case LogicalOr:
        case Not:
        case LEqual:
        case GEqual:
        case GThan:
        case Assign: /* Because LEqual and GEqual might not be parsed. */
        case Equal:
        case LThan:
        case NotEqual:
        case Modulo:
        case BitwiseNot:
          if (!in_argument) {
            tree.open_scope(curr, ScopeType::FunctionParam);
            in_argument = true;
          }
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  void attribute_or_subscript()
  {
    if (curr.next() == '[') {
      attribute();
    }
    else {
      subscript();
    }
  }

  void subscript()
  {
    tree.open_scope(curr, ScopeType::Subscript);
    match('[');

    while (true) {
      switch (peek()) {
        case Hash:
          preprocessor();
          break;
        case ParOpen:
          function_call_or_local_parenthesis();
          break;
        case SquareOpen:
          subscript();
          break;
        case SquareClose:
          tree.close_scope(curr, ScopeType::Subscript);
          match(']');
          return;
        case Minus:
        case Plus:
        case Dot:
        case Multiply:
        case Colon:
        case Question:
        case Ampersand:
        case Word:
        case Number:
        case Divide:
        case LogicalAnd:
        case LogicalOr:
        case GThan:
        case LThan:
        case GEqual:
        case LEqual:
        case Not:
        case NotEqual:
        case Modulo:
        case BitwiseNot:
        case Or:
        case Xor:
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  void attribute()
  {
    tree.open_scope(curr, ScopeType::Subscript);
    match('[');
    tree.open_scope(curr, ScopeType::Attributes);
    match('[');

    bool in_attribute = false;
    while (true) {
      switch (peek()) {
        case SquareClose:
          if (in_attribute) {
            in_attribute = false;
            tree.close_scope(curr.prev(), ScopeType::Attribute);
          }
          tree.close_scope(curr, ScopeType::Attributes);
          match(']');
          tree.close_scope(curr, ScopeType::Subscript);
          match(']');
          /* Attributes can be chained. */
          if (peek() == '[') {
            attribute();
          }
          return;
        case ParOpen:
          function_call();
          break;
        case Comma:
          if (in_attribute) {
            in_attribute = false;
            tree.close_scope(curr.prev(), ScopeType::Attribute);
          }
          next();
          break;
        case Word:
          if (!in_attribute) {
            tree.open_scope(curr, ScopeType::Attribute);
            in_attribute = true;
          }
          next();
          break;
        default:
          error("Unexpected token");
          return;
      }
    }
  }

  /* Example: `namespace A::B {}`. */
  void namespace_declaration()
  {
    match(Namespace);
    qualified_id();
    tree.open_scope(curr, ScopeType::Namespace);
    match('{');
    external_declaration();
    tree.close_scope(curr, ScopeType::Namespace);
    match('}');
  }

  /* Example: `A::B`. */
  void qualified_id()
  {
    match(Word);
    while (peek() == ':') {
      match(':');
      match(':');
      match(Word);
    }
  }

  /* Example: `#define A\n`. */
  void preprocessor()
  {
    const LexerBase &lex = parser;
    tree.open_scope(curr, ScopeType::Preprocessor);

    int tok_id = curr.index_;
    while (true) {
      const TokenType type = lex.types_[tok_id];
      if (type == EndOfFile) {
        tok_id--;
        break;
      }
      std::string_view tok_str = lex[tok_id].str_with_whitespace();
      size_t new_line = -1;
      while ((new_line = tok_str.find("\n", new_line + 1)) != std::string::npos) {
        if (new_line == 0 || tok_str[new_line - 1] != '\\') {
          break;
        }
      }
      if (new_line != std::string::npos) {
        break;
      }
      tok_id++;
    }
    curr = lex[tok_id];
    tree.close_scope(curr, ScopeType::Preprocessor);
    next();
  }
};

}  // namespace blender::gpu::shader::parser
