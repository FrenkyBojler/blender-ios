/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "lazy_string_builder.hh"
#include "shader_tool/intermediate.hh"

namespace blender::gpu {

struct DeadCodeEliminator
    : shader::parser::IntermediateForm<shader::parser::SimpleLexer, shader::parser::NullParser> {
  using Token = shader::parser::Token;
  using TokenType = shader::parser::TokenType;

 private:
  /* Function ID that is unique for each function and all its overloads. */
  using FnId = int;

  struct FunctionGraph {
    /* Counter to assign unique IDs to functions. */
    int counter = 0;
    /* Map declarations (name token) to a function id. */
    Vector<std::pair<Token, FnId>> declarations;
    /* Map identifier to id. */
    Map<StringRef, FnId> names;
    /* Function call (from, to). */
    Vector<std::pair<FnId, FnId>> edges;
  } graph;

  FnId current_fn_id;

  /* Disable function declaration processing.
   * However, still process function calls. */
  bool parsing_enabled;

  /* TODO(fclem): Meh find a better way. Exceptions? */
  static void report_fn(int /*error_line*/,
                        int /*error_char*/,
                        std::string /*error_line_string*/,
                        const char * /*error_str*/)
  {
    BLI_assert_unreachable();
  }
  shader::parser::report_callback report_fn_ptr = report_fn;

 public:
  DeadCodeEliminator(const std::string_view str)
      : shader::parser::IntermediateForm<shader::parser::SimpleLexer, shader::parser::NullParser>(
            str, report_fn_ptr)
  {
  }

  /* Fetch previous token skipping whitespace. */
  static Token prev(Token tok)
  {
    tok = tok.prev();
    while (tok == shader::parser::Space || tok == shader::parser::NewLine) {
      tok = tok.prev();
    }
    return tok;
  }

  /* Fetch next token skipping whitespace. */
  static Token next(Token tok)
  {
    tok = tok.next();
    while (tok == shader::parser::Space || tok == shader::parser::NewLine) {
      tok = tok.next();
    }
    return tok;
  }

  static Token find_matching_pair(Token start, TokenType scope_open, TokenType scope_close)
  {
    int stack = 1;
    Token tok = start;
    while (tok.is_valid()) {
      tok = next(tok);
      if (tok == scope_open) {
        stack++;
        continue;
      }
      if (tok == scope_close) {
        stack--;
        if (stack == 0) {
          return tok;
        }
      }
    }
    BLI_assert_unreachable();
    return Token::invalid();
  }

  void function_definition(Token name_tok, Token par_tok)
  {
    StringRef name = str(name_tok);
    FnId &id = graph.names.lookup_or_add(name, -1);

    if (id == -1) {
      id = graph.counter++;
    }

    graph.declarations.append_as(name_tok, id);

    Token end_of_args = find_matching_pair(par_tok, TokenType::ParOpen, TokenType::ParClose);

    if (next(end_of_args) == '{') {
      current_fn_id = id;
    }
  }

  void function_call(Token name_tok)
  {
    if (current_fn_id == -1) {
      return;
    }

    StringRef name = str(name_tok);

    int fn_id = graph.names.lookup_default(name, -1);
    /* TODO(fclem): On Metal, the function prototypes are removed, which means they can be defined
     * later on.  */
    if (fn_id == -1) {
      /* Functions is not defined. Can be builtin function. */
      return;
    }
    graph.edges.append_as(current_fn_id, fn_id);
  }

  /* There can be a few remaining directive. Avoid parsing them as functions. */
  void process_function(int &cursor)
  {
    Token parenthesis_tok = parser_[cursor];
    Token name_tok = prev(parenthesis_tok);
    /* WATCH(fclem): It could be that a line directive is put between the return type and the
     * function name (which would mess up the). This is currently not happening with the
     * current codebase but might in the future. Checking for it would be quite expensive. */
    if (name_tok != TokenType::Word) {
      return;
    }
    Token type_tok = prev(name_tok);
    StringRef type_str = str(type_tok);

    TokenType type_tok_type = type_tok.type();
    if (type_tok == TokenType::Word && type_str[0] >= '0' && type_str[0] <= '9') {
      /* Case where a function is called just after a line directive. The type token was not
       * recognized as a Number token from the tokenizer rules. */
      type_tok_type = TokenType::Number;
    }

    if (type_tok_type == TokenType::Word && type_str != "return" && type_str != "else") {
      if (parsing_enabled) {
        function_definition(name_tok, parenthesis_tok);
      }
    }
    else {
      function_call(name_tok);
    }
  }

  /* There can be a few remaining directive. Avoid parsing them as functions. */
  void process_directives(int &cursor)
  {
    Token hash_tok = parser_[cursor];
    Token dir_name = next(hash_tok);
    Token end_tok = end_of_directive(dir_name);
    cursor = end_tok.index;

    StringRef whole_dir_str = substr_range_inclusive_view(dir_name, end_tok);

    if (whole_dir_str == "pragma blender dead_code_elimination off") {
      parsing_enabled = false;
    }
    else if (whole_dir_str == "pragma blender dead_code_elimination on") {
      parsing_enabled = true;
    }
  }

  void parse_source()
  {
    current_fn_id = -1;
    parsing_enabled = true;

    int stack_depth = 0;

    for (int cursor = 0; cursor < lex_.token_types.size(); cursor++) {
      TokenType tok_type = TokenType(lex_.token_types[cursor]);
      if (tok_type == TokenType::ParOpen) {
        process_function(cursor);
      }
      else if (tok_type == TokenType::Hash) {
        process_directives(cursor);
      }
      else if (current_fn_id != -1) {
        if (tok_type == TokenType::BracketOpen) {
          stack_depth++;
        }
        else if (tok_type == TokenType::BracketClose) {
          stack_depth--;
          if (stack_depth == 0) {
            current_fn_id = -1;
          }
        }
      }
    }
  }

  Map<FnId, Vector<FnId>> build_adjacency()
  {
    Map<FnId, Vector<FnId>> adj;
    adj.reserve(graph.counter);
    for (const auto &[from, to] : graph.edges) {
      adj.lookup_or_add_default(from).append(to);
    }
    return adj;
  }

  Set<FnId> compute_used_functions(const Vector<FnId> &roots)
  {
    Set<FnId> used;
    used.reserve(graph.counter);

    auto adj = build_adjacency();

    std::vector<FnId> stack;
    stack.reserve(64);

    for (FnId root : roots) {
      if (used.add(root)) {
        stack.push_back(root);
      }

      while (!stack.empty()) {
        FnId f = stack.back();
        stack.pop_back();

        const auto *calls = adj.lookup_ptr(f);
        if (calls == nullptr) {
          continue;
        }

        for (FnId callee : *calls) {
          if (used.add(callee)) {
            stack.push_back(callee);
          }
        }
      }
    }

    return used;
  }

  void prune_unused_functions()
  {
    FnId main_id = graph.names.lookup_default("main", -1);
    if (main_id == -1) {
      /* Can be true inside tests. */
      return;
    }

    Vector<FnId> entry_points{graph.names.lookup("main")};
    /* TODO(fclem): Properly support forward declaration. */
    if (graph.names.contains("nodetree_displacement")) {
      entry_points.append(graph.names.lookup("nodetree_displacement"));
    }
    if (graph.names.contains("nodetree_surface")) {
      entry_points.append(graph.names.lookup("nodetree_surface"));
    }
    if (graph.names.contains("nodetree_volume")) {
      entry_points.append(graph.names.lookup("nodetree_volume"));
    }
    if (graph.names.contains("nodetree_thickness")) {
      entry_points.append(graph.names.lookup("nodetree_thickness"));
    }
    if (graph.names.contains("derivative_scale_get")) {
      entry_points.append(graph.names.lookup("derivative_scale_get"));
    }
    if (graph.names.contains("closure_to_rgba")) {
      entry_points.append(graph.names.lookup("closure_to_rgba"));
    }

    Set<FnId> used = compute_used_functions(entry_points);

    for (auto [name_tok, id] : graph.declarations) {
      if (used.contains(id)) {
        continue;
      }
      Token type = prev(name_tok);
      Token parenthesis = next(name_tok);
      Token end_of_args = find_matching_pair(parenthesis, TokenType::ParOpen, TokenType::ParClose);
      Token body_start = next(end_of_args);
      if (body_start == '{') {
        /* Full definition. */
        Token body_end = find_matching_pair(
            body_start, TokenType::BracketOpen, TokenType::BracketClose);
        erase(type, body_end);
      }
      else {
        /* Prototype. */
        /* Filter MSL & GLSL specific identifiers that could have confused the parser. */
        StringRef type_str = str(type);
        StringRef name_str = str(name_tok);
        if (type_str == "thread" || type_str == "device" || name_str == "layout") {
          continue;
        }
        erase(type, next(end_of_args));
      }
    }
  }

  void optimize()
  {
    parse_source();
    prune_unused_functions();
  }

  static StringRef str(const Token t)
  {
    /* Note: Whitespaces where not merged (because of TokenizePreprocessor), so using
     * str_view_with_whitespace will be faster.  */
    return t.str_view_with_whitespace();
  }

  static Token end_of_directive(const Token dir_tok)
  {
    Token tok = dir_tok;

    while (tok != TokenType::NewLine) {
      if (tok.next() == TokenType::Invalid) {
        /* Error or end of file. */
        return tok;
      }
      tok = skip_directive_newlines(tok.next());
    }
    return tok.prev();
  }

  static Token skip_directive_newlines(Token tok)
  {
    while (tok == '\\' && tok.next() == '\n') {
      tok = tok.next().next();
    }
    return tok;
  }
};

/* -------------------------------------------------------------------- */
/** \name Streaming Dead Code Eliminator.
 * \{ */

/* Token stream that parses (some) symbols definitions, build a graph of usage and then prune
 * unused definitions for a given set of entry point functions. */
struct DCEStream : private LazyStringBuilder {
  using Token = lexit::Token;
  using TokenType = lexit::TokenType;
  using TokenAtom = lexit::TokenAtom;

 private:
  struct TokenFingerPrint {
    /* Start of this token inside the LazyStringBuilder result. */
    uint32_t str_start = 0;
    TokenAtom atom = 0;
    TokenType type = TokenType::Invalid;
    uint8_t size = 0;

    TokenFingerPrint() = default;
  };

  /* Simple circular buffer to access last few token data. */
  class TokenHistoryBuffer {
   private:
    std::array<TokenFingerPrint, 3> buffer = {TokenFingerPrint{}};
    int head = 0;

   public:
    void push(TokenFingerPrint item)
    {
      buffer[head] = item;
      head = (head + 1) % buffer.size();
    }

    /* Access elements from newest (0) to oldest (count-1). */
    const TokenFingerPrint &operator[](int index) const
    {
      BLI_assert(index < buffer.size());
      int actual_index = (head + buffer.size() - index - 1) % buffer.size();
      return buffer[actual_index];
    }
  } token_history;

  /* Function ID that is unique for each function and all its overloads. */
  using FnId = int;

  struct FunctionDeclaration {
    TokenFingerPrint type, name, end;
    FnId id;
  };

  struct FunctionGraph {
    /* Counter to assign unique IDs to functions. */
    int counter = 0;
    /* Map declarations (name token) to a function id. */
    Vector<FunctionDeclaration> declarations;
    /* Map identifier to id. */
    Map<TokenAtom, FnId> names;
    /* Function call (from, to). */
    Vector<std::pair<FnId, FnId>> edges;
  } graph;

  FnId current_fn_id = -1;

  /* State of the parser. Some section have DCE turned off because of unsupported syntax. */
  bool enabled_ = true;

  int stack_depth = 0;

  const TokenAtom return_atom;

 public:
  DCEStream(TokenAtom return_atom) : return_atom(return_atom) {}

  void set_enabled_parsing(bool value)
  {
    enabled_ = value;
  }

  DCEStream &operator<<(StringRef str)
  {
    *static_cast<LazyStringBuilder *>(this) << str;
    return *this;
  }

  BLI_NOINLINE void parse_token(TokenAtom atom, TokenType type)
  {
    if (!enabled_) {
      return;
    }
    parse_token_impl(atom, type);
  }

  BLI_NOINLINE void parse_token(const Token start, const Token end)
  {
    if (!enabled_) {
      return;
    }
    int offset = 0;
    for (Token tok = start; tok != end; tok = tok.next()) {
      parse_token_impl(tok.atom(), tok.type(), offset);
      offset += tok.str_with_whitespace().size();
    }
  }

  std::string str() const
  {
    return static_cast<const LazyStringBuilder *>(this)->str();
  }

  void optimize(const Span<TokenAtom> entry_points)
  {
    prune_unused_functions(entry_points);
  }

 private:
  BLI_INLINE_METHOD void parse_token_impl(TokenAtom atom, TokenType type, int offset = 0)
  {
    TokenFingerPrint tok{static_cast<uint32_t>(this->total_length + offset), atom, type, 0};
    switch (type) {
      case TokenType::ParOpen:
        process_function();
        break;
      case TokenType::BracketOpen:
        stack_depth += (current_fn_id != -1);
        break;
      case TokenType::BracketClose:
        stack_depth -= (current_fn_id != -1);
        if (stack_depth == 0 && current_fn_id != -1) {
          graph.declarations.last().end = tok;
          current_fn_id = -1;
        }
        break;
      case TokenType::SemiColon:
        /* Finding a semicolon in global scope after a function signature means that this is
         * a forward declaration. Step out of function in this case. */
        if (stack_depth == 0 && current_fn_id != -1) {
          graph.declarations.last().end = tok;
          current_fn_id = -1;
        }
        break;
      default:
        break;
    }
    token_history.push(tok);
  }

  void process_function()
  {
    TokenFingerPrint name_tok = token_history[0];
    if (name_tok.type != TokenType::Word) {
      return;
    }

    TokenFingerPrint type_tok = token_history[1];
    if (type_tok.type == TokenType::Word && type_tok.atom != return_atom) {
      register_function_declaration(type_tok, name_tok);
    }
    else if (current_fn_id == -1) {
      register_function_call(name_tok);
    }
  }

  /* Register function declaration at this token position.
   * Associate ID with the token string if first encountering the symbol.
   * Does not differentiate overloads. */
  void register_function_declaration(TokenFingerPrint type_tok, TokenFingerPrint name_tok)
  {
    FnId id = graph.names.lookup_or_add_cb(name_tok.atom, [this]() { return graph.counter++; });
    graph.declarations.append(FunctionDeclaration{type_tok, name_tok, name_tok, id});
    current_fn_id = id;
  }

  /* Register a function call made inside the body of a function by creating an edge inside the
   * graph. Does nothing if the function is not defined. */
  void register_function_call(TokenFingerPrint name_tok)
  {
    int fn_id = graph.names.lookup_default(name_tok.atom, -1);
    /* TODO(fclem): On Metal, the function prototypes are removed, which means they can be defined
     * later on.  */
    if (fn_id == -1) {
      /* Functions is not defined. Can be builtin function. */
      return;
    }
    graph.edges.append_as(current_fn_id, fn_id);
  }

  void end_function_declaration(TokenFingerPrint tok)
  {
    if (stack_depth == 0 && current_fn_id != -1) {
      graph.declarations.last().end = tok;
      current_fn_id = -1;
    }
  }

  Map<FnId, Vector<FnId>> build_adjacency()
  {
    Map<FnId, Vector<FnId>> adj;
    adj.reserve(graph.counter);
    for (const auto &[from, to] : graph.edges) {
      adj.lookup_or_add_default(from).append(to);
    }
    return adj;
  }

  Set<FnId> compute_used_functions(const Vector<FnId> &roots)
  {
    Set<FnId> used;
    used.reserve(graph.counter);

    auto adj = build_adjacency();

    std::vector<FnId> stack;
    stack.reserve(64);

    for (FnId root : roots) {
      if (used.add(root)) {
        stack.push_back(root);
      }

      while (!stack.empty()) {
        FnId f = stack.back();
        stack.pop_back();

        const auto *calls = adj.lookup_ptr(f);
        if (calls == nullptr) {
          continue;
        }

        for (FnId callee : *calls) {
          if (used.add(callee)) {
            stack.push_back(callee);
          }
        }
      }
    }

    return used;
  }

  void prune_unused_functions(const Span<TokenAtom> entry_points)
  {
    Vector<FnId> entry_point_ids;
    for (auto entry_point : entry_points) {
      FnId id = graph.names.lookup_default(entry_point, 0);
      if (id != 0) {
        entry_point_ids.append(id);
      }
    }

    if (entry_point_ids.is_empty()) {
      /* Can be true inside tests. */
      return;
    }

    Set<FnId> used = compute_used_functions(entry_points);

    for (auto [type_tok, name_tok, end_tok, id] : graph.declarations) {
      if (used.contains(id)) {
        continue;
      }

      // std::cout << name_tok.str_start << std::endl;

      // if (type_tok.atom == thread_atom || type_tok.atom == device_atom ||
      //     name_tok.atom == layout_atom)
      // {
      //   /* Filter MSL & GLSL specific identifiers that could have confused the parser. */
      //   continue;
      // }

      // erase(type_tok, end_tok);
    }
  }
};

/** \} */

}  // namespace blender::gpu
