/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "shader_tool/expression.hh"
#include "shader_tool/intermediate.hh"

#include "gpu_shader_private.hh"

#define XXH_INLINE_ALL
#include "shader_tool/xxhash.hh"

namespace blender::gpu {

using namespace shader::parser;

uint32_t XXH3_str(StringRef str)
{
  return XXH3_64bits(str.data(), str.size());
}

/**
 * Consider numbers as words (to avoid splitting identifiers).
 * Does not merge newlines and spaces.
 * Convert all identifier strings (words) into unique identifiers (AtomT) for fast comparison.
 */
struct AtomicLexer : LexerBase {
  Map<uint32_t, AtomT> atomization_map;

  void lexical_analysis(std::string_view input)
  {
    str = input;
    ensure_memory();
    tokenize(true);
    if (str.size() > 1000) {
      atomize_words();
      build_line_structure();
    }
  }

  void atomize_words()
  {
    /* From checking our statistics. This heuristic should be enough for 99% of our cases. */
    atomization_map.reserve(token_types.size() / 10);

#ifndef NDEBUG
    Map<StringRef, uint32_t> check_map;
    check_map.reserve(token_types.size() / 10);
#endif

    for (int tok_id : blender::IndexRange(token_types.size())) {
      if (token_types[tok_id] == Word) {
        IndexRange range = token_offsets[tok_id];
        StringRef substr(str.data() + range.start, range.size);
        uint32_t hash = XXH3_str(substr);
#ifndef NDEBUG
        check_map.add_or_modify(
            substr,
            [hash](uint32_t *value) { *value = hash; },
            [hash](const uint32_t *value) { BLI_assert(*value == hash); });
#endif
        token_atoms[tok_id] = atomization_map.lookup_or_add(hash, atomization_map.size());
      }
    }
    token_atoms.shrink(token_types.size());
  }

  /* OffsetIndices in tokens. */
  Vector<int> line_offsets_buf;
  blender::OffsetIndices<int> line_offsets;

  /* Line index */
  Vector<int> directive_lines;

  void build_line_structure()
  {
    /* From checking our statistics. This heuristic should be enough for 100% of our cases. */
    line_offsets_buf.reserve(token_types.size() / 7);
    directive_lines.reserve(line_offsets_buf.size() / 2);

    line_offsets_buf.append(0);
    int tok_id = 0;
    for (TokenType type : blender::Span<TokenType>(token_types.data(), token_types.size())) {
      tok_id++;
      if (type == NewLine) {
        line_offsets_buf.append(tok_id);
      }
      else if (type == Hash) {
        int line_start = line_offsets_buf.last();
        /* Directive can only start with a hash token (+ optional space).
         * If there is more token before the hash token it cannot be a preprocessor directive. */
        if (tok_id - line_start <= 1) {
          int line_index = line_offsets_buf.size() - 1;
          if (directive_lines.is_empty() || directive_lines.last() != line_index) {
            directive_lines.append(line_index);
          }
        }
      }
    }
    line_offsets_buf.append(tok_id);

    line_offsets = line_offsets_buf.as_span();
  }
};

/* TODO(fclem): Meh find a better way. Exceptions? */
void report_fn(int /*error_line*/,
               int /*error_char*/,
               std::string /*error_line_string*/,
               const char * /*error_str*/)
{
  BLI_assert_unreachable();
}

report_callback report_fn_ptr = report_fn;

/* Fast C (incomplete) preprocessor implementation.  */
struct Preprocessor : IntermediateForm<AtomicLexer, DummyParser> {
  using ExpansionParser = IntermediateForm<ExpansionLexer, DummyParser>;

  struct TokenTrait {};
  struct LineTrait {};
  struct DirectiveTrait {};

  using AtomT = AtomicLexer::AtomT;

  enum DirectiveType : char {
    /* Any other unhandled directives (warnings / errors / pragma etc...). */
    Define = 0,
    Undef,
    Line,
    If,
    Ifdef,
    Ifndef,
    Elif,
    Else,
    Endif,
    Other,
  };

  AtomT directive_type_table[Other] = {
      [Define] = AtomT(lex_.atomization_map.lookup_default(XXH3_str("define"), -1)),
      [Undef] = AtomT(lex_.atomization_map.lookup_default(XXH3_str("undef"), -1)),
      [Line] = AtomT(lex_.atomization_map.lookup_default(XXH3_str("line"), -1)),
      [If] = AtomT(lex_.atomization_map.lookup_default(XXH3_str("if"), -1)),
      [Ifdef] = AtomT(lex_.atomization_map.lookup_default(XXH3_str("ifdef"), -1)),
      [Ifndef] = AtomT(lex_.atomization_map.lookup_default(XXH3_str("ifndef"), -1)),
      [Elif] = AtomT(lex_.atomization_map.lookup_default(XXH3_str("elif"), -1)),
      [Else] = AtomT(lex_.atomization_map.lookup_default(XXH3_str("else"), -1)),
      [Endif] = AtomT(lex_.atomization_map.lookup_default(XXH3_str("endif"), -1)),
  };

  template<typename T> class ID {
#ifndef NDEBUG
   public:
    std::string_view str;
#endif
   private:
    int id_;

   public:
    ID() = delete;

    explicit ID(int i) : id_(i) {}

    static ID invalid()
    {
      return ID(-1);
    }

    operator int() const
    {
      return id_;
    }

    friend bool operator==(ID a, ID b)
    {
      return a.id_ == b.id_;
    }
    friend bool operator!=(ID a, ID b)
    {
      return a.id_ == b.id_;
    }
  };

  using TokenID = ID<TokenTrait>;
  using LineID = ID<LineTrait>;
  using DirectiveID = ID<DirectiveTrait>;

  bool is_valid(TokenID tok)
  {
    return int(tok) >= 0 && int(tok) < lex_.token_types.size();
  }
  bool is_valid(LineID line)
  {
    return int(line) >= 0 && int(line) < lex_.line_offsets.size();
  }
  bool is_valid(DirectiveID dir)
  {
    return int(dir) >= 0 && int(dir) < lex_.directive_lines.size();
  }

  TokenID make_token(int index)
  {
    TokenID tok(index);
#ifndef NDEBUG
    tok.str = str(tok);
#endif
    BLI_assert(is_valid(tok));
    return tok;
  }
  LineID make_line(int index)
  {
    LineID line(index);
#ifndef NDEBUG
    line.str = str(line);
#endif
    BLI_assert(is_valid(line));
    return line;
  }
  DirectiveID make_directive(int index)
  {
    DirectiveID dir(index);
#ifndef NDEBUG
    dir.str = str(dir);
#endif
    BLI_assert(is_valid(dir));
    return dir;
  }

  LineID next(LineID line)
  {
    return make_line(int(line) + 1);
  }
  LineID prev(LineID line)
  {
    return make_line(int(line) - 1);
  }

  TokenID next(TokenID token)
  {
    return make_token(int(token) + 1);
  }
  TokenID prev(TokenID token)
  {
    return make_token(int(token) - 1);
  }

  DirectiveID next(DirectiveID directive)
  {
    return make_directive(int(directive) + 1);
  }
  DirectiveID prev(DirectiveID directive)
  {
    return make_directive(int(directive) - 1);
  }

  LineID get_start(DirectiveID dir)
  {
    return make_line(lex_.directive_lines[dir]);
  }
  LineID get_end(DirectiveID dir)
  {
    /* Could be precomputed if becoming a bottleneck. */
    LineID line = get_start(dir);
    while (get_type(get_end(line)) == Backslash) {
      line = next(line);
    }
    return line;
  }

  TokenID get_start(LineID line)
  {
    return make_token(lex_.line_offsets[line].start());
  }
  /* NOTE: Return the token before \n. */
  TokenID get_end(LineID line)
  {
    blender::IndexRange range = lex_.line_offsets[line];
    return make_token(range.size() > 1 ? range.last(1) : range.first());
  }
  TokenType get_type(TokenID tok)
  {
    return lex_.token_types[tok];
  }
  AtomT get_atom(TokenID tok)
  {
    return lex_.token_atoms[tok];
  }

  StringRef str(DirectiveID dir)
  {
    LineID start = get_start(dir);
    LineID end = get_end(dir);
    Token tok_start = parser_[get_start(start)];
    Token tok_end = parser_[get_end(end)];
    return substr_range_inclusive_view(tok_start, tok_end);
  }
  StringRef str(LineID line)
  {
    TokenID start = get_start(line);
    TokenID end = get_end(line);
    Token tok_start = parser_[start];
    Token tok_end = parser_[end];
    return substr_range_inclusive_view(tok_start, tok_end);
  }
  StringRef str(TokenID tok)
  {
    return parser_[tok].str_view_with_whitespace();
  }
  StringRef str(TokenID start, TokenID end_inclusive)
  {
    return substr_range_inclusive_view(parser_[start], parser_[end_inclusive]);
  }

  /* Return token defining the directive type (e.g. define, undef, if ...). */
  TokenID get_identifier(DirectiveID dir)
  {
    LineID line = get_start(dir);
    TokenID hash_tok = skip_space(get_start(line));
    BLI_assert(get_type(hash_tok) == Hash);
    TokenID dir_tok = skip_space(next(hash_tok));
    BLI_assert(get_type(dir_tok) == Word);
    return dir_tok;
  }

  AtomT get_hash(DirectiveID dir)
  {
    return lex_.token_atoms[get_identifier(dir)];
  }

  DirectiveType get_type(DirectiveID dir)
  {
    return to_directive_type(get_hash(dir));
  }

  Preprocessor(const std::string_view str)
      : IntermediateForm<AtomicLexer, DummyParser>(str, report_fn_ptr)
  {
  }

  struct TokenRange {
    TokenID start, end;
  };

  Vector<DirectiveID, 8> jump_stack;
  /* Maps macro names to definition name token index. */
  Set<AtomT> visited_macros;

  /* Own stack to avoid memory allocation during recursive expansion parsing. */
  struct ParserStack {
    int allocated = 0;
    int used = 0;
    std::deque<ExpansionParser> parser_pool;

    ExpansionParser &alloc()
    {
      if (used == allocated) {
        parser_pool.emplace_back("", report_fn_ptr);
        allocated++;
        used++;
        return parser_pool.back();
      }
      return parser_pool[used++];
    }

    void release(ExpansionParser & /*parser*/)
    {
      used--;
    }
  } recursive_parser_stack;

  /* Cache the expression lexer to avoid memory allocations. */
  ExpressionLexer expression_lexer;
  ExpressionParser expression_parser = ExpressionParser(expression_lexer);

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

  DirectiveType to_directive_type(const AtomT id_hash)
  {
    for (int i : blender::IndexRange(Other)) {
      if (directive_type_table[i] == id_hash) {
        return DirectiveType(i);
      }
    }
    return Other;
  }

  static StringRef str(const Token t)
  {
    /* Note: Whitespaces where not merged (because of TokenizePreprocessor), so using
     * str_view_with_whitespace will be faster.  */
    return t.str_view_with_whitespace();
  }

  StringRef str(const TokenRange &range)
  {
    return str(range.start, range.end);
  }

  std::string new_lines(LineID line_start, LineID line_end)
  {
    return std::string(line_end - line_start, '\n');
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

  TokenID skip_directive_newlines(TokenID tok)
  {
    /* TODO make it safe */
    while (lex_.token_types[tok] == '\\' && lex_.token_types[tok + 1] == '\n') {
      tok = next(next(tok));
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

#if 0
  /* Try to match the token pointed at by cursor with a defined macro.
   * If that happen advance the cursor to the end of the macro (in case of functional macro). */
  void try_expand(MutableString &mut_str, const ParserBase &data, TokenID &cursor)
  {
    TokenID tok = Token::from_position(&data, cursor);
    StringRef tok_str = str(tok);
    /* Early out number literals.
     * Anything below '0' is not an alphabetical character and thus cannot start a word.
     * Saves one comparison. */
    if (tok_str[0] <= '9') {
      return;
    }

    int macro_id = defines.lookup_default(str(tok), -1);
    if (macro_id != -1) {
      TokenID macro_tok = macro_id;
      auto [replacement, end] = expand_macro(tok, macro_tok);
      mut_str.replace(tok, end, replacement);
      cursor = end.index;
    }
  }

  /* Parse and expand with the current set of macro identifier. */
  std::string parse_and_expand(StringRef input)
  {
    if (input.is_empty()) {
      return "";
    }

    ExpansionParser &recursive_parser = recursive_parser_stack.alloc();

    recursive_parser.str_ = input;
    recursive_parser.parse(report_fn_ptr);

    const ParserBase &data = recursive_parser.data_get();

    for (int cursor = 0; cursor < data.lex.token_types.size(); cursor++) {
      TokenType tok_type = TokenType(data.lex.token_types[cursor]);
      if (tok_type == Word) {
        try_expand(recursive_parser, data, cursor);
      }
    }

    std::string result = recursive_parser.result_get(true);

    recursive_parser_stack.release(recursive_parser);

    return result;
  }

  struct ExpandedResult {
    /* Replacement content. */
    std::string str;
    /* End of range to replace. */
    TokenID end_of_expansion;
  };

  ExpandedResult expand_macro(const TokenID expanded_tok, const TokenID macro_name)
  {
    TokenID tok = next(macro_name);
    const bool is_function = (get_type(tok) == '(');

    TokenID end_of_expansion = expanded_tok;

    tok = skip_space(tok);

    /* Empty definition. */
    if (tok == '\n') {
      return {"", end_of_expansion};
    }

    if (visited_macros.contains(get_atom(macro_name))) {
      /* Recursion. Do not expand. Still replace by the original token. */
      return {str(macro_name), end_of_expansion};
    }

    Map<StringRef, TokenRange> macro_parameters;
    if (is_function) {
      /* This is a functional macro. */

      TokenID param = skip_space(expanded_tok.next());
      if (param != '(') {
        /* Macro doesn't have parameters. It should not expand. */
        return {str(macro_name), end_of_expansion};
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
#endif

  /* Returns the type of conditional. */
  DirectiveType increment_to_next_conditional(DirectiveID &dir)
  {
    dir = next(dir);
    while (is_valid(dir)) {
      DirectiveType type = get_type(dir);
      if (ELEM(type, If, Ifdef, Ifndef, Else, Elif, Endif)) {
        return type;
      }
      dir = next(dir);
    }
    /* Missing matching #endif. */
    // TODO exception?
    BLI_assert_unreachable();
    return Other;
  }

  /* Returns the hash token. */
  DirectiveID find_next_matching_conditional(DirectiveID dir)
  {
    int stack = 1;
    while (is_valid(dir)) {
      DirectiveType type = increment_to_next_conditional(dir);
      if (ELEM(type, If, Ifdef, Ifndef)) {
        stack++;
      }
      else if (ELEM(type, Endif)) {
        stack--;
      }

      if (stack == 0) {
        return dir; /* Endif. */
      }
      if (stack == 1 && ELEM(type, Else, Elif)) {
        return dir;
      }
    }
    BLI_assert_unreachable();
    return DirectiveID::invalid();
  }

  AtomT defined_atom = lex_.atomization_map.lookup_default(XXH3_str("defined"), -1);

  bool evaluate_expression(const TokenID start, const TokenID end)
  {
    /* Expand expression into integer ops string. */
    std::string expand;
    expand.reserve(256);

    std::string_view s = substr_range_inclusive_view(parser_[start], parser_[end]);

    TokenID tok = start;
    while (true) {
      AtomT tok_atom = get_atom(tok);

      // TokenID macro_id = defines_tok.lookup_default(tok_atom, TokenID::invalid());
      //  if (macro_id != -1) {
      //    auto [replacement, macro_end] = expand_macro(parser_[tok], parser_[macro_id]);
      //    expand += replacement;
      //    tok = make_token(macro_end.index);
      //  }
      //  else
      if (tok_atom == defined_atom) {
        /* Parenthesis or space */
        tok = skip_space(next(tok));
        const bool is_function = (get_type(tok) == '(');
        /* Token to search. */
        if (is_function) {
          tok = skip_space(next(tok));
        }
        expand += (defines_tok.contains(get_atom(tok)) ? '1' : '0');
        if (is_function) {
          /* End parenthesis. */
          tok = skip_space(next(tok));
        }
      }
      else {
        expand += str(tok);
      }
      if (tok == end) {
        break;
      }
      tok = skip_directive_newlines(next(tok));
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
      expression_lexer.lexical_analysis(expand);
      value = expression_parser.eval();
    }
    catch (const std::exception &e) {
      std::cout << "\"" << str(start, end) << "\" > \"" << expand << "\" ";
      std::cerr << "Error: " << e.what() << "\n";
    }

    return value != 0;
  }

  bool evaluate_condition(const DirectiveType dir_type, TokenID start, TokenID end)
  {
    switch (dir_type) {
      case Else:
        return true;
      case Ifdef:
        return defines_tok.contains(get_atom(start));
      case Ifndef:
        return !defines_tok.contains(get_atom(start));
      case If:
      case Elif:
        return evaluate_expression(start, end);
      default:
        BLI_assert_unreachable();
        return true;
    }
  }

  void process_conditional(const DirectiveID dir, const DirectiveType dir_type)
  {
    /* If this is part of an already evaluated statement. */
    if (!jump_stack.is_empty() && jump_stack.last() == dir) {
      jump_stack.pop_last();

      DirectiveID endif = next_directive;
      while (get_type(endif) != Endif) {
        endif = find_next_matching_conditional(endif);
      }
      LineID endif_end = get_end(endif);
      /* Erase everything between this directive and the #endif (inclusive). */
      // erase_lines(get_start(dir), endif_end);
      /* Evaluate after the endif */
      // next_directive = next(endif);
      return;
    }

    const LineID dir_line_start = get_start(dir);
    const LineID dir_line_end = get_end(dir);
    const TokenID dir_tok = get_identifier(dir);
    /* Evaluate condition. */
    const TokenID cond_start = skip_space(next(dir_tok));
    const TokenID cond_end = get_end(dir_line_end);
    const bool condition_result = evaluate_condition(dir_type, cond_start, cond_end);

    /* Find matching endif or else. */
    const DirectiveID next_condition = find_next_matching_conditional(dir);

    if (condition_result) {
      /* If is followed by else statement. */
      DirectiveType next_dir_type = get_type(next_condition);
      if (ELEM(next_dir_type, Elif, Else)) {
        /* Record a jump statement at the next #else statement to jump & erase to the #endif. */
        jump_stack.append(next_condition);
      }
      /* Erase condition and continue parsing content.
       * The #endif will just be erased later. */
      // erase_lines(dir_line_start, dir_line_end);
    }
    else {
      /* Erase the content and jump to next condition. */
      // next_directive = next_condition;
      /* Erase everything until next condition (this directive included). */
      // erase_lines(dir_line_start, prev(get_start(next_directive)));
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

  // /* Check if '/' token is the start of a comment and remove the comment if it is.
  //  * Some runtime sources could still contain comments. */
  // void process_comment(const ParserBase &data, int &cursor)
  // {
  //   TokenType next_type = data[cursor].next().type();
  //   size_t end;

  //   if (next_type == '*') {
  //     /* Multiline line. */
  //     end = data.lex.token_types_str.find("*/", cursor);
  //     if (end == std::string::npos) {
  //       /* Extend until end of file. */
  //       end = data.lex.token_types_str.size() - 2;
  //     }
  //     end += 1;
  //   }
  //   else if (next_type == '/') {
  //     /* Single line. */
  //     end = data.lex.token_types_str.find('\n', cursor);
  //     if (end == std::string::npos) {
  //       /* Extend until end of file. */
  //       end = data.lex.token_types_str.size() - 1;
  //     }
  //   }
  //   else {
  //     return;
  //   }

  //   parser.erase(data[cursor], data[end]);
  //   cursor = end;
  // }

  DirectiveID next_directive = DirectiveID::invalid();

  Map<AtomT, TokenID> defines_tok;

  void define_macro(DirectiveID dir)
  {
    TokenID macro_name = skip_space(next(get_identifier(dir)));
    BLI_assert(get_type(macro_name) == Word);
    /* Store the name token of the declaration.
     * The actual parsing of the definition happens during expansion. */
    defines_tok.add_overwrite(get_atom(macro_name), macro_name);
  }

  void undefine_macro(DirectiveID dir)
  {
    TokenID macro_name = skip_space(next(get_identifier(dir)));
    BLI_assert(get_type(macro_name) == Word);
    defines_tok.remove(get_atom(macro_name));
  }

  TokenID skip_space(TokenID tok)
  {
    return (get_type(tok) == Space) ? next(tok) : tok;
  }

  void evaluate_directive(DirectiveID dir)
  {
    DirectiveType dir_type = get_type(dir);

    bool erase_directive = true;
    switch (dir_type) {
      case Define:
        define_macro(dir);
        erase_directive = false;
        break;
      case Undef:
        undefine_macro(dir);
        erase_directive = false;
        break;
      case If:
      case Ifdef:
      case Ifndef:
      case Elif:
      case Else:
        process_conditional(dir, dir_type);
        erase_directive = false; /* Erases itself. */
        break;
      case Line:
        break;
      case Endif:
        erase_directive = false;
        break;
      case Other:
        erase_directive = false;
        break;
    }

    if (erase_directive == true) {
      erase_lines(get_start(dir), get_end(dir));
    }
  }

  void erase_lines(LineID start, LineID end)
  {
    /* Last char is newline. Don't remove it. */
    Token tok_end = parser_[lex_.line_offsets[end].last() - 1];
    Token tok_start = parser_[lex_.line_offsets[start].start()];

    replace(tok_start, tok_end, new_lines(start, end));
  }

  void preprocess()
  {
    next_directive = make_directive(0);
    while (next_directive < lex_.directive_lines.size() - 1) {
      DirectiveID id = next_directive;
      /* The next directive might be overwritten by evaluate_directive. Increment before call. */
      next_directive = next(id);
      evaluate_directive(id);
    }
    /* Evaluate last directive without calling next and creating an invalid ID. */
    evaluate_directive(next_directive);

    // for (int cursor = 0; cursor < data.lex.token_types.size(); cursor++) {
    //   TokenType tok_type = TokenType(data.lex.token_types[cursor]);
    // if (tok_type == Word) {
    // try_expand(parser, data, cursor);
    // }
    // else
    // if (tok_type == Hash) {
    //   process_directives(data, cursor);
    // }
    // else if (tok_type == Divide) {
    //   process_comment(data, cursor);
    // }
    // }
  }
};

#if 0
struct DeadCodeEliminator {
  PreprocessorParser &parser;

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

  /* Fetch previous token skipping whitespace. */
  static Token prev(Token tok)
  {
    tok = tok.prev();
    while (tok == Space || tok == NewLine) {
      tok = tok.prev();
    }
    return tok;
  }

  /* Fetch next token skipping whitespace. */
  static Token next(Token tok)
  {
    tok = tok.next();
    while (tok == Space || tok == NewLine) {
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
    StringRef name = Preprocessor::str(name_tok);
    FnId &id = graph.names.lookup_or_add(name, -1);

    if (id == -1) {
      id = graph.counter++;
    }

    graph.declarations.append_as(name_tok, id);

    Token end_of_args = find_matching_pair(par_tok, ParOpen, ParClose);

    if (next(end_of_args) == '{') {
      current_fn_id = id;
    }
  }

  void function_call(Token name_tok)
  {
    if (current_fn_id == -1) {
      return;
    }

    StringRef name = Preprocessor::str(name_tok);

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
  void process_function(const ParserBase &data, int &cursor)
  {
    Token parenthesis_tok = data[cursor];
    Token name_tok = prev(parenthesis_tok);
    /* WATCH(fclem): It could be that a line directive is put between the return type and the
     * function name (which would mess up the). This is currently not happening with the
     * current codebase but might in the future. Checking for it would be quite expensive. */
    if (name_tok != Word) {
      return;
    }
    Token type_tok = prev(name_tok);
    StringRef type_str = Preprocessor::str(type_tok);

    TokenType type_tok_type = type_tok.type();
    if (type_str[0] >= '0' && type_str[0] <= '9') {
      /* Case where a function is called just after a line directive. The type token was not
       * recognized as a Number token from the tokenizer rules. */
      type_tok_type = Number;
    }

    if (type_tok_type == Word && type_str != "return" && type_str != "else") {
      if (parsing_enabled) {
        function_definition(name_tok, parenthesis_tok);
      }
    }
    else {
      function_call(name_tok);
    }
  }

  /* There can be a few remaining directive. Avoid parsing them as functions. */
  void process_directives(const ParserBase &data, int &cursor)
  {
    Token hash_tok = data[cursor];
    Token dir_name = next(hash_tok);
    Token end_tok = Preprocessor::end_of_directive(dir_name);
    cursor = end_tok.index;

    StringRef whole_dir_str = parser.substr_range_inclusive_view(dir_name, end_tok);

    if (whole_dir_str == "pragma blender dead_code_elimination off") {
      parsing_enabled = false;
    }
    else if (whole_dir_str == "pragma blender dead_code_elimination on") {
      parsing_enabled = true;
    }
  }

  void parse_source()
  {
    const ParserBase &data = parser.data_get();

    current_fn_id = -1;
    parsing_enabled = true;

    int stack_depth = 0;

    for (int cursor = 0; cursor < data.lex.token_types.size(); cursor++) {
      TokenType tok_type = TokenType(data.lex.token_types[cursor]);
      if (tok_type == ParOpen) {
        process_function(data, cursor);
      }
      else if (tok_type == Hash) {
        process_directives(data, cursor);
      }
      else if (current_fn_id != -1) {
        if (tok_type == BracketOpen) {
          stack_depth++;
        }
        else if (tok_type == BracketClose) {
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

    Set<FnId> used = compute_used_functions(entry_points);

    for (auto [name_tok, id] : graph.declarations) {
      if (used.contains(id)) {
        continue;
      }
      Token type = prev(name_tok);
      Token parenthesis = next(name_tok);
      Token end_of_args = find_matching_pair(parenthesis, ParOpen, ParClose);
      Token body_start = next(end_of_args);
      if (body_start == '{') {
        /* Full definition. */
        Token body_end = find_matching_pair(body_start, BracketOpen, BracketClose);
        parser.erase(type, body_end);
      }
      else {
        /* Prototype. */
#  ifdef __APPLE__
        /* Filter MSL specific identifiers that could have confused the parser. */
        StringRef type_str = Preprocessor::str(type);
        if (type_str == "thread" || type_str == "device") {
          continue;
        }
#  endif
        parser.erase(type, end_of_args);
      }
    }
  }

  void optimize()
  {
    parse_source();
    prune_unused_functions();
  }
};
#endif

std::string Shader::run_preprocessor(StringRef source)
{
  Preprocessor processor(source);
  processor.preprocess();

  // parser.apply_mutations(true);

  // DeadCodeEliminator dce{parser};
  // dce.optimize();

  return processor.result_get(true);
}

}  // namespace blender::gpu
