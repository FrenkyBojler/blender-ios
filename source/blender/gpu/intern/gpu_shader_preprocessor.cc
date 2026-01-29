/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_global.hh"
#include "BLI_struct_equality_utils.hh"

#include "shader_tool/expression.hh"
#include "shader_tool/intermediate.hh"

#include "gpu_shader_dead_code_elimination.hh"
#include "gpu_shader_private.hh"

#include "shader_tool/lexit/lexit.hh"
#include "shader_tool/lexit/tables.hh"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Utilities.
 * \{ */

namespace shader::parser {

struct TokenRange {
  lexit::Token start, end;
};

}  // namespace shader::parser

/** \} */

using namespace shader::parser;

/* -------------------------------------------------------------------- */
/** \name Parser / Lexer classes.
 * \{ */

/**
 * Lexer variant for very fast tokenization for the preprocessor.
 * Consider numbers as words (to avoid splitting and then merging later on).
 * Does not merge newlines and spaces together.
 * Convert all identifier strings (words) into unique identifiers (TokenAtom) for fast comparison.
 */
struct AtomicLexer : LexerBase {
  /* Line index to token range. */
  blender::OffsetIndices<int> line_offsets;
  /* Preprocessor directive to line index. */
  Vector<int> directive_lines;

  void lexical_analysis(std::string_view input)
  {
    str = input;
    process(input, lexit::char_class_table);
    merge_spaces();

    token_types_str = std::string_view((const char *)types_.get(), size_);
    token_types = {types_.get(), size_};
    token_offsets = {offsets_.get(), size_ + 1};

    lex_pass();
  }

  BLI_INLINE_METHOD TokenAtom hash(StringRef tok_str)
  {
    switch (tok_str.size()) {
      case 1:
        /* Reserve [0-127] range for single char token. */
        return tok_str[0];
      case 2:
        /* Reserve [128-16511] range for double char token. tok_str[1] cannot be 0. */
        return tok_str[0] + tok_str[1] * uint16_t(128);
      default:
        /* Long identifier slow path. Do full hash */
        return atomization_map_.lookup_or_add_cb(tok_str, [this]() { return this->next_hash(); });
    }
  }

 protected:
  constexpr BLI_INLINE char perfect_hash(std::string_view s)
  {
    return s.size() * 3 + (s[0] - s.back());
  }

  BLI_INLINE TokenType type_lookup(std::string_view s)
  {
    switch (perfect_hash(s)) {
      case perfect_hash("if"):
        return (s == "if") ? If : Word;
      case perfect_hash("elif"):
        return (s == "elif") ? Elif : Word;
      case perfect_hash("else"):
        return (s == "else") ? Else : Word;
      case perfect_hash("line"):
        return (s == "line") ? Line : Word;
      case perfect_hash("endif"):
        return (s == "endif") ? Endif : Word;
      case perfect_hash("ifdef"):
        return (s == "ifdef") ? Ifdef : Word;
      case perfect_hash("undef"):
        return (s == "undef") ? Undef : Word;
      case perfect_hash("define"):
        return (s == "define") ? Define : Word;
      case perfect_hash("ifndef"):
        return (s == "ifndef") ? Ifndef : Word;
      default:
        return Word;
    }
  }

  /** Map string hashes to atom value. */
  Map<StringRef, TokenAtom> atomization_map_;
  /* Reserve [16512-65536] range for longer token. */
  uint16_t atom_hash_counter_ = 16512;

  uint16_t next_hash()
  {
    /* Check for overflow. */
    BLI_assert(atom_hash_counter_ >= 16512);
    return atom_hash_counter_++;
  }

  /* Backing buffer for line_offsets. */
  Vector<int> line_offsets_buf_;

  /**
   * All-in-one lexing pass.
   * - Keywords identification.
   * - Identifiers atomization.
   * - Line structure building.
   */
  BLI_NOINLINE void lex_pass()
  {
    /* From checking our statistics. This heuristic should be enough for 99% of our cases. */
    atomization_map_.reserve(token_types.size() / 17);
    /* From checking our statistics. This heuristic should be enough for 100% of our cases. */
    line_offsets_buf_.reserve(token_types.size() / 7);
    directive_lines.reserve(line_offsets_buf_.size() / 2);

    line_offsets_buf_.append(0);
    for (auto tok : *this) {
      switch (tok.type()) {
        case Word: {
          tok.type() = type_lookup(tok.str());
          if (tok.type() == Word) {
            atoms_[int(tok)] = hash(tok.str());
          }
          break;
        }
        case NewLine: {
          line_offsets_buf_.append(int(tok) + 1);
          break;
        }
        case '#': {
          int line_start = line_offsets_buf_.last();
          /* Directive can only start with a hash token (+ optional space).
           * If there is more token before the hash token it cannot be a preprocessor directive. */
          if (int(tok) - line_start <= 1) {
            int line_index = line_offsets_buf_.size() - 1;
            if (directive_lines.is_empty() || directive_lines.last() != line_index) {
              directive_lines.append(line_index);
            }
          }
          break;
        }
        default:
          break;
      }
    }
    /* Finish last line. But only do so if it contains at least one character. */
    if (line_offsets_buf_.last() != size()) {
      line_offsets_buf_.append(size());
    }

    line_offsets = line_offsets_buf_.as_span();
  }
};

struct ExpansionLexer : LexerBase {
  void lexical_analysis(std::string_view input)
  {
    str = input;
    process(input, lexit::char_class_table);
    merge_spaces();

    token_types_str = std::string_view((const char *)types_.get(), size_);
    token_types = {types_.get(), size_};
    token_offsets = {offsets_.get(), size_ + 1};
  }
};

struct Stream : TokenBuffer {
  AtomicLexer &lex;

  std::string str;

  bool concat_next = false;

  struct Space {};
  struct Number {
    StringRef str;
  };
  struct ConcatNext {};

  Stream(AtomicLexer &lex) : TokenBuffer(), lex(lex)
  {
    str.reserve(512);
    this->reserve(256);
    offsets_[0] = 0;
    original_offsets_[0] = 0;
    whitespaces_collapsed_ = true;
  }

  Stream(Stream &&s) : lex(s.lex)
  {
    this->str = std::move(s.str);
    this->types_ = std::move(s.types_);
    this->offsets_ = std::move(s.offsets_);
    this->original_offsets_ = std::move(s.original_offsets_);
    this->atoms_ = std::move(s.atoms_);
  }

  void clear()
  {
    str.clear();
    static_cast<TokenBuffer *>(this)->clear();
  }

  Stream &operator<<(const Stream &stream)
  {
    for (int i : stream.index_range()) {
      *this << stream[i];
    }
    return *this;
  }

  Stream &operator<<(lexit::Token tok)
  {
    ensure_space_for_one();
    str += tok.str();
    /* When concatenating, keep type of the previous token. */
    if (!concat_next) {
      types_[size_] = tok.type();
    }
    atoms_[size_] = tok.atom();
    size_++;
    original_offsets_[size_] = str.size();
    if (tok.followed_by_whitespace()) {
      str += ' ';
    }
    str_ = str;
    offsets_[size_] = str.size();

    if (concat_next) {
      /* Update Atom of the new pasted token. */
      atoms_[size_ - 1] = lex.hash((*this)[size_ - 1].str());
      concat_next = false;
    }
    return *this;
  }

  /* NOTE: Not compatible with concatenation. */
  Stream &operator<<(Number tok)
  {
    ensure_space_for_one();
    str += tok.str;
    str_ = str;
    types_[size_] = TokenType::Number;
    size_++;
    original_offsets_[size_] = str.size();
    offsets_[size_] = str.size();
    return *this;
  }

  Stream &operator<<(const TokenRange &stream)
  {
    for (lexit::Token tok = stream.start; int(tok) <= int(stream.end); tok = tok.next()) {
      *this << tok;
    }
    /* "Cancel" concatenation in case range is empty. */
    if (concat_next) {
      /* Data of the previous token is still there. Just recover it ... */
      size_ += 1;
      /* ... except for the trailing space. */
      offsets_[size_] = original_offsets_[size_];
      concat_next = false;
    }
    return *this;
  }

  Stream &operator<<(ConcatNext /*concat*/)
  {
    BLI_assert_msg(!concat_next, "Token concatenation followed by another concatenation");
    if (size_ == 0 || concat_next) {
      /* Nothing to concatenate. */
      return *this;
    }
    /* If last char is a space, remove it to effectively concatenate.
     * Note that this assumes that there is only at most one space between tokens. */
    if (str.back() == ' ') {
      str.pop_back();
      str_ = str;
    }
    /* Pop last token. Next token will override its end offset but not its start. */
    size_ -= 1;
    concat_next = true;
    return *this;
  }

  Stream &operator<<(Space /*space*/)
  {
    ensure_space_for_one();
    if (!str.empty() && str.back() != ' ') {
      str += ' ';
      str_ = str;
      offsets_[size_] = str.size();
    }
    return *this;
  }

  blender::IndexRange index_range() const
  {
    return blender::IndexRange(size_);
  }

  void ensure_space_for_one()
  {
    if (UNLIKELY(size_ + 1 >= allocated_size_)) {
      this->reserve(allocated_size_ + 256);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Type-safe identifier management.
 * \{ */

/* Simple integer identifier with a debug string view.
 * Allow type safety and function overload. */
template<typename Trait, typename T = int> class ID {
#ifndef NDEBUG
 public:
  std::string_view str;
#endif
 private:
  T id_;

 public:
  ID() = delete;

  explicit ID(T i) : id_(i) {}

  static ID invalid()
  {
    return ID(-1);
  }

  explicit operator T() const
  {
    return id_;
  }

  BLI_STRUCT_EQUALITY_OPERATORS_1(ID, id_)

  uint64_t hash() const
  {
    return id_;
  }
};

/* TODO(fclem): Meh find a better way. Exceptions? */
static void report_fn(int /*error_line*/,
                      int /*error_char*/,
                      std::string /*error_line_string*/,
                      const char * /*error_str*/)
{
  BLI_assert_unreachable();
}

report_callback report_fn_ptr = report_fn;

/**
 * Boiler plate class exposing lexer structure using typed IDs.
 */
struct IntermediateFormWithIDs : IntermediateForm<AtomicLexer, NullParser> {

  IntermediateFormWithIDs(StringRef str)
      : IntermediateForm<AtomicLexer, NullParser>(str, report_fn_ptr)
  {
  }

  struct TokenTrait {};
  struct LineTrait {};
  struct DirectiveTrait {};
  struct AtomTrait {};

  using LineID = ID<LineTrait>;
  using DirectiveID = ID<DirectiveTrait>;
  /* Typesafe Atom. */
  using AtomID = ID<AtomTrait, TokenAtom>;

  enum DirectiveType : char {
    Define = TokenType::Define,
    Undef = TokenType::Undef,
    Line = TokenType::Line,
    If = TokenType::If,
    Ifdef = TokenType::Ifdef,
    Ifndef = TokenType::Ifndef,
    Elif = TokenType::Elif,
    Else = TokenType::Else,
    Endif = TokenType::Endif,
    /* Any other unhandled directives (warnings / errors / pragma etc...). */
    Other = TokenType::Word,
  };

  /* Cached 'defined' keyword identifier. */
  AtomID defined_atom = get_atom("defined");

  /**
   * Validity check.
   */
  bool is_valid(LineID line)
  {
    return int(line) >= 0 && int(line) < lex_.line_offsets.size();
  }
  bool is_valid(DirectiveID dir)
  {
    return int(dir) >= 0 && int(dir) < lex_.directive_lines.size();
  }

  /**
   * Check if item is the last of its kind.
   */
  bool is_last(DirectiveID dir)
  {
    return (lex_.directive_lines.size() - 1) == int(dir);
  }
  bool is_last(LineID line)
  {
    return (lex_.line_offsets.size() - 1) == int(line);
  }

  /**
   * Creation. Creating an invalid token is undefined behavior.
   */
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

  /**
   * Convert ID to string.
   */
  StringRef str(DirectiveID dir)
  {
    LineID start = get_start(dir);
    LineID end = get_end(dir);
    lexit::Token tok_start = lex_[int(get_start(start))];
    lexit::Token tok_end = lex_[int(get_end(end))];
    return substr_range_inclusive_view(tok_start, tok_end);
  }
  StringRef str(LineID line)
  {
    lexit::Token start = get_start(line);
    lexit::Token end = get_end(line);
    return substr_range_inclusive_view(start, end);
  }

  /* Return valid value if tok is valid and a word token. */
  AtomID get_atom(lexit::Token tok)
  {
    BLI_assert(tok.type() == Word);
    return AtomID(lex_.atoms_[int(tok)]);
  }
  /* Return valid value if hash is a known string. Is full hash lookup + hashing. */
  AtomID get_atom(StringRef str)
  {
    return AtomID(lex_.hash(str));
  }

  /**
   * Return next token. Result in undefined behavior if id is last.
   */
  LineID next(LineID line)
  {
    return make_line(int(line) + 1);
  }
  DirectiveID next(DirectiveID directive)
  {
    return make_directive(int(directive) + 1);
  }

  /**
   * Return previous token. Result in undefined behavior if id is first.
   */
  LineID prev(LineID line)
  {
    return make_line(int(line) - 1);
  }
  DirectiveID prev(DirectiveID directive)
  {
    return make_directive(int(directive) - 1);
  }

  /**
   * Return the start element.
   */
  lexit::Token get_start(LineID line)
  {
    return lex_[lex_.line_offsets[int(line)].start()];
  }
  LineID get_start(DirectiveID dir)
  {
    return make_line(lex_.directive_lines[int(dir)]);
  }

  /**
   * Return the end element.
   * NOTE: Return the token before \n or \n if line is empty.
   */
  lexit::Token get_end(LineID line)
  {
    blender::IndexRange range = lex_.line_offsets[int(line)];
    return lex_[range.size() > 1 ? range.last(1) : range.last()];
  }
  LineID get_end(DirectiveID dir)
  {
    /* Could be precomputed if becoming a bottleneck. */
    LineID line = get_start(dir);
    while (get_end(line) == Backslash) {
      line = next(line);
    }
    return line;
  }

  /* NOTE: Return the end of line character '\n'. */
  lexit::Token get_true_end(LineID line)
  {
    return lex_[lex_.line_offsets[int(line)].last()];
  }

  /**
   * Get the corresponding type enum.
   */
  DirectiveType get_type(DirectiveID dir)
  {
    return DirectiveType(get_identifier(dir).type());
  }

  /* Return token defining the directive type (e.g. define, undef, if ...). */
  lexit::Token get_identifier(DirectiveID dir)
  {
    LineID line = get_start(dir);
    lexit::Token hash_tok = get_start(line);
    BLI_assert(hash_tok == Hash);
    lexit::Token dir_tok = hash_tok.next();
    return dir_tok;
  }

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
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preprocessor.
 * \{ */

/* Fast C (incomplete) preprocessor implementation.  */
struct Preprocessor : IntermediateFormWithIDs {
 private:
  using ExpansionParser = IntermediateForm<ExpansionLexer, DummyParser>;
  using TokenRange = shader::parser::TokenRange;

  /* Cache the expression lexer to avoid memory allocations. */
  ExpressionLexer expression_lexer;
  ExpressionParser expression_parser = ExpressionParser(expression_lexer);

  struct StreamPool {
    /* Reference to lexer only for atom value lookups. */
    AtomicLexer &lex_;
    int allocated = 0;
    int used = 0;
    std::deque<Stream> pool;
    std::vector<int> free_indices;

    struct Deleter {
      StreamPool *stack;
      int index;

      void operator()(Stream * /*stream*/)
      {
        stack->free_indices.push_back(index);
      }
    };

    using Ptr = std::unique_ptr<Stream, Deleter>;

    Ptr alloc()
    {
      int target_idx;
      if (free_indices.empty()) {
        target_idx = pool.size();
        pool.emplace_back(lex_);
      }
      else {
        target_idx = free_indices.back();
        free_indices.pop_back();
      }

      Stream &s = pool[target_idx];
      s.clear();
      return Ptr(&s, Deleter{this, target_idx});
    }
  };

  using StreamPtr = StreamPool::Ptr;

  /* When evaluating a condition directive inside this stack, disregard the directive and jump to
   * the matching #endif. */
  Vector<DirectiveID, 8> jump_stack;
  /* Own stack to avoid memory allocation during recursive expansion parsing. */
  StreamPool stream_pool = {lex_};
  /* Set of visited macros during recursion (blue painting stack). Using a vector for speed. */
  Vector<DirectiveID> visited_macros;
  /* Maps containing currently active macros. Map their keyword to their definition. */
  Map<AtomID, DirectiveID> defines;

  /**
   * State Tracking.
   */

  /* Next preprocessor directive to evaluate. Might be overwritten by conditional evaluation. */
  DirectiveID next_directive = DirectiveID::invalid();
  /* End of the last evaluated directive. Might be overwritten by conditional evaluation.
   * Used to resume token expansion after this line. */
  LineID last_directive_end = LineID::invalid();

 public:
  Preprocessor(const std::string_view str) : IntermediateFormWithIDs(str)
  {
    /* From our stats. Should be enough for 100% of our cases. */
    defines.reserve(1000);
  }

  void preprocess()
  {
    if (lex_.directive_lines.is_empty()) {
      return;
    }

    last_directive_end = make_line(0);
    next_directive = make_directive(0);
    /* Expand until the first directive. */
    if (make_line(0) != get_start(next_directive)) {
      expand_macros_in_range(make_line(0), prev(get_start(next_directive)));
    }

    while (!is_last(next_directive)) {
      DirectiveID id = next_directive;
      /* The next directive might be overwritten by evaluate_directive. Increment before call. */
      next_directive = next(id);
      evaluate_directive(id);

      expand_macros_in_range(next(last_directive_end), prev(get_start(next_directive)));
    }
    /* Evaluate last directive without calling next and creating an invalid ID. */
    evaluate_directive(next_directive);

    if (!is_last(last_directive_end)) {
      LineID last_line = make_line(lex_.line_offsets.size() - 1);
      expand_macros_in_range(next(last_directive_end), last_line);
    }
  }

 private:
  void evaluate_directive(DirectiveID dir)
  {
    DirectiveType dir_type = get_type(dir);

    /* Note: gets overwritten by conditional processing. */
    last_directive_end = get_end(dir);

    bool erase_directive = true;
    switch (dir_type) {
      case Define:
        define_macro(dir);
        break;
      case Undef:
        undefine_macro(dir);
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
        break;
      case Other:
        erase_directive = false;
        break;
    }

    if (erase_directive == true) {
      erase_lines(get_start(dir), get_end(dir));
    }
  }

  /**
   * Macro Management.
   */

  void define_macro(DirectiveID dir)
  {
    lexit::Token macro_name = get_identifier(dir).next();
    BLI_assert(macro_name == Word);
    /* Store the name token of the declaration.
     * The actual parsing of the definition happens during expansion. */
    defines.add_overwrite(get_atom(macro_name), dir);
  }

  void undefine_macro(DirectiveID dir)
  {
    lexit::Token macro_name = get_identifier(dir).next();
    BLI_assert(macro_name == Word);
    defines.remove(get_atom(macro_name));
  }

  /**
   * Condition directives.
   */

  void process_conditional(const DirectiveID dir, const DirectiveType dir_type)
  {
    /* If this is part of an already evaluated statement. */
    if (!jump_stack.is_empty() && jump_stack.last() == dir) {
      jump_stack.pop_last();
      /* Find matching endif. */
      DirectiveID endif = find_next_matching_conditional(dir);
      while (get_type(endif) != Endif) {
        endif = find_next_matching_conditional(endif);
      }
      if (is_last(endif)) {
        /* Erase everything this and the last directive. */
        LineID last_before_endif = prev(get_start(endif));
        erase_lines(get_start(dir), last_before_endif);
        next_directive = endif;
        /* Don't expand inside this section. */
        last_directive_end = last_before_endif;
      }
      else {
        /* Erase everything between this directive and the #endif (inclusive). */
        LineID endif_end = get_end(endif);
        erase_lines(get_start(dir), endif_end);
        /* Evaluate after the endif. */
        next_directive = next(endif);
        /* Don't expand inside this section. */
        last_directive_end = endif_end;
      }
      return;
    }

    const LineID dir_line_start = get_start(dir);
    const LineID dir_line_end = get_end(dir);
    const lexit::Token dir_tok = get_identifier(dir);
    /* Evaluate condition. */
    const lexit::Token cond_start = dir_tok.next();
    const lexit::Token cond_end = get_end(dir_line_end);
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
      erase_lines(dir_line_start, dir_line_end);
    }
    else {
      LineID last_before_next_cond = prev(get_start(next_condition));
      /* Erase everything until next condition (this directive included). */
      erase_lines(dir_line_start, last_before_next_cond);
      /* Jump to next condition. */
      next_directive = next_condition;
      /* Don't expand inside this section. */
      last_directive_end = last_before_next_cond;
    }
  }

  bool evaluate_condition(const DirectiveType dir_type, lexit::Token start, lexit::Token end)
  {
    switch (dir_type) {
      case Else:
        return true;
      case Ifdef:
        return defines.contains(get_atom(start));
      case Ifndef:
        return !defines.contains(get_atom(start));
      case If:
      case Elif:
        return evaluate_expression(start, end);
      default:
        BLI_assert_unreachable();
        return true;
    }
  }

  bool evaluate_expression(const lexit::Token start, const lexit::Token end)
  {
#ifndef NDEBUG
    /* For debugging. */
    std::string_view original_expr = substr_range_inclusive_view(parser_[int(start)],
                                                                 parser_[int(end)]);
    UNUSED_VARS(original_expr);
#endif

    /* Expand expression into integer ops string. */
    StreamPtr expand = expand_expression(start, end);

    /* Early out simple cases. */
    if (expand->size() == 1 && (*expand)[0].str() == "0") {
      return false;
    }
    if (expand->size() == 1 && (*expand)[0].str() == "1") {
      return true;
    }

    try {
      /* TODO(fclem): Do not parse again. Simply use the token stream. */
      expression_lexer.lexical_analysis(expand->str);
      return expression_parser.eval() != 0;
    }
    catch (const std::exception &e) {
      std::cout << "\"" << substr_range_inclusive_view(start, end) << "\" > \"" << expand->str
                << "\" ";
      std::cerr << "Error: " << e.what() << "\n";
      return false;
    }
  }

  /**
   * Macro Expansion.
   */

  void expand_macros_in_range(const LineID start_line, const LineID end_line)
  {
    int start = int(get_start(start_line));
    int end = int(get_true_end(end_line));
    if (start > end) {
      return;
    }

    for (lexit::Token tok = lex_[start], end_tok = lex_[end]; tok != end_tok; tok = tok.next()) {
      if (tok == Word) {
        DirectiveID macro_id = defines.lookup_default(get_atom(tok), DirectiveID::invalid());
        if (is_valid(macro_id)) {
          lexit::Token token = parser_.lex[int(tok)];
          auto [replacement, end] = expand_macro(token, macro_id);
          replace(token, end, replacement->str);
          tok = end;
          if (tok == end_tok) {
            break;
          }
        }
      }
    }
  }

  /* Parse and expand with the current set of macro identifier. */
  StreamPtr parse_and_expand(const Stream &ts)
  {
    auto result = stream_pool.alloc();

    for (int cursor = 0, end = ts.index_range().size(); cursor < end; cursor++) {
      lexit::Token tok = ts[cursor];
      if (tok.type() == Word) {
        /* Try to match the token pointed at by cursor with a defined macro. If that happen advance
         * the cursor to the end of the macro (in case of functional macro). */
        DirectiveID macro_id = defines.lookup_default(AtomID(tok.atom()), DirectiveID::invalid());
        if (is_valid(macro_id)) {
          auto [replacement, end] = expand_macro(tok, macro_id);
          *result << *replacement;
          cursor = int(end);
          continue;
        }
      }
      *result << tok;
    }
    return result;
  }

  struct ExpandedResult {
    /* Replacement content. */
    StreamPtr output;
    /* End of range to replace. */
    lexit::Token end_of_expansion;
  };

  BLI_NOINLINE Map<TokenAtom, TokenRange> parse_macro_args(const bool is_function,
                                                           const lexit::Token expanded_tok,
                                                           lexit::Token &end_of_expansion,
                                                           lexit::Token &tok)
  {
    Map<TokenAtom, TokenRange> macro_parameters;
    if (is_function) {
      /* This is a functional macro. */

      lexit::Token param = expanded_tok.next();
      if (param != ParOpen) {
        /* Macro doesn't have parameters. It should not expand. */
        return {};
      }

      /* Parse parameters & arguments. */
      macro_parameters.clear_and_keep_capacity();
      while (tok != ParClose) {
        /* Continue to the next name. */
        tok = tok.next();
        if (tok == ParClose) {
          /* Function with no arguments. */
          param = get_end_of_parameter(param);
          if (param == Invalid) {
            /* Error: missing closing parenthesis. */
            /* Cancel expansion. */
            return {};
          }
          if (param != ParClose) {
            /* Error: too many arguments provided to function-like macro invocation. */
            /* Cancel expansion. */
            return {};
          }
          break;
        }

        lexit::Token param_start = param;
        lexit::Token param_end = get_end_of_parameter(param_start);

        StringRef argument_name = tok.str();
        TokenAtom atom;
        if (argument_name == "...") {
          param_end = get_end_of_parameter(param_start, true);
          argument_name = "__VA_ARGS__";
          atom = lex_.hash("__VA_ARGS__");
        }
        else {
          atom = lex_.atoms_[int(tok)];
        }

        /* If there is only token for parameters (it could be empty string). */
        if (param_start.next() == param_end.prev()) {
          macro_parameters.add(atom, {param_start.next(), param_start.next()});
        }
        else {
          macro_parameters.add(atom, {param_start.next(), param_end.prev()});
        }

        /* Continue to the next separator. */
        tok = tok.next();
        param = param_end;

        if (tok == Invalid) {
          break;
        }
      }
      /* Skip closing parenthesis. */
      tok = tok.next();
      /* Make sure to replace the whole call. */
      end_of_expansion = param;
    }
    return macro_parameters;
  }

  BLI_NOINLINE StreamPtr parse_macro_definition(lexit::Token definition_start_tok)
  {
    StreamPtr definition = stream_pool.alloc();

    lexit::Token tok = definition_start_tok;
    while (true) {
      lexit::Token tok_next = tok.next();
      if (ELEM(tok, NewLine, lexit::EndOfFile)) {
        break;
      }
      if (tok == Backslash && tok_next == NewLine) {
        /* Preprocessor new line. Skip and continue. */
        tok = tok.next(2);
        /* Still insert a space to avoid merging tokens. */
        *definition << Stream::Space{};
        continue;
      }
      if (tok == Hash && tok_next == Hash) {
        /* Token Pasting operator. Emit a single Hash token. */
        *definition << tok;
        tok = tok.next(2);
        continue;
      }
      BLI_assert_msg(tok != Hash, "Stringify operator is not supported");
      *definition << tok;
      tok = tok_next;
    }
    return definition;
  }

  BLI_NOINLINE void expand_args(Stream &ts,
                                const bool is_function,
                                const Map<TokenAtom, TokenRange> &macro_parameters,
                                lexit::Token definition_start_tok)
  {
    StreamPtr definition = parse_macro_definition(definition_start_tok);

    for (lexit::Token def_tok : *definition) {
      if (def_tok.type() == Hash) {
        ts << Stream::ConcatNext{};
        continue;
      }
      if (is_function && def_tok.type() == Word) {
        /* Lookup macro arguments. */
        const TokenRange *macro_value_ptr = macro_parameters.lookup_ptr(def_tok.atom());
        if (macro_value_ptr) {
          const TokenRange &macro_value = *macro_value_ptr;

          if (def_tok.prev() == Hash || def_tok.next() == Hash) {
            /* Token pasting. Do not expand now. */
            ts << macro_value;
          }
          else {
            /* Expand argument. Can expand to the same macro (finite recursion). */
            StreamPtr stream = stream_pool.alloc();
            *stream << macro_value;
            ts << *parse_and_expand(*stream);
          }

          if (def_tok.followed_by_whitespace()) {
            /* Don't lose whitespace after macro token. */
            ts << Stream::Space{};
          }
          continue;
        }
      }
      ts << def_tok;
    }
  }

  /**
   * IMPORTANT: Because of recursion, expanded_tok can be from another parser.
   * The macro directive however, will always be from the main parser.
   */
  ExpandedResult expand_macro(const lexit::Token expanded_tok, const DirectiveID macro)
  {
    const lexit::Token define_tok = get_identifier(macro);
    const lexit::Token macro_name = define_tok.next();
    const lexit::Token macro_parenthesis = macro_name.next();
    BLI_assert(define_tok == TokenType::Define);
    BLI_assert(macro_name == Word);

    const bool is_function = (macro_parenthesis == ParOpen) &&
                             !macro_name.followed_by_whitespace();

    lexit::Token end_of_expansion = expanded_tok;

    lexit::Token tok = macro_parenthesis;

    StreamPtr expanded = stream_pool.alloc();

    /* Empty definition. */
    if (tok == NewLine) {
      return {std::move(expanded), end_of_expansion};
    }

    if (visited_macros.contains(macro)) {
      /* Recursion. Do not expand. Still replace by the original token. */
      *expanded << lex_[int(macro_name)];
      return {std::move(expanded), end_of_expansion};
    }

    Map<TokenAtom, TokenRange> macro_parameters = parse_macro_args(
        is_function, expanded_tok, end_of_expansion, tok);

    expand_args(*expanded, is_function, macro_parameters, tok);

    /* Add to the set to avoid infinite recursion. */
    visited_macros.append(macro);

    StreamPtr result = parse_and_expand(*expanded);

    visited_macros.pop_last();

    if (end_of_expansion.followed_by_whitespace()) {
      *result << Stream::Space{};
    }

    return {std::move(result), end_of_expansion};
  }

  /* Expand token range for condition evaluation (e.g. '#if'). */
  StreamPtr expand_expression(const lexit::Token start, const lexit::Token end)
  {
    StreamPtr result = stream_pool.alloc();

    lexit::Token tok = start;
    while (true) {
      BLI_assert(tok.is_valid());
      AtomID tok_atom = tok == Word ? get_atom(tok) : AtomID::invalid();

      DirectiveID macro = defines.lookup_default(tok_atom, DirectiveID::invalid());

      if (tok_atom == AtomID::invalid()) {
        /* Non word. */
        *result << lex_[int(tok)];
      }
      else if (is_valid(macro)) {
        auto [replacement, macro_end] = expand_macro(lex_[int(tok)], macro);
        *result << *replacement;
        tok = lex_[int(macro_end)];
      }
      else if (tok_atom == defined_atom) {
        /* Parenthesis or space */
        tok = tok.next();
        const bool is_function = (tok == ParOpen);
        /* Token to search. */
        if (is_function) {
          tok = tok.next();
        }
        else {
          BLI_assert(tok == Word);
        }
        *result << Stream::Number{defines.contains(get_atom(tok)) ? "1" : "0"};
        if (is_function) {
          /* End parenthesis. */
          tok = tok.next();
        }
      }
      else {
        /* Substitution failure. */
        *result << lex_[int(tok)];
      }
      if (tok == end) {
        break;
      }
      tok = skip_directive_newlines(tok.next());
    }

    return result;
  }

  /**
   * Utilities.
   */

  void erase_lines(LineID start, LineID end)
  {
    Token tok_end = parser_[int(get_end(end))];
    Token tok_start = parser_[int(get_start(start))];
    replace(tok_start, tok_end, new_lines(start, end));
  }

  /* Return a string with the amount of newline character between line_start and line_end. */
  std::string new_lines(LineID line_start, LineID line_end)
  {
    return std::string(int(line_end) - int(line_start), '\n');
  }

  lexit::Token skip_directive_newlines(lexit::Token tok)
  {
    while (tok == Backslash && tok.next() == NewLine) {
      tok = tok.next().next();
    }
    return tok;
  }

  /**
   * Return next `,` or `)` skipping occurrences contained in parenthesis.
   * Return invalid token on failure.
   */
  static lexit::Token get_end_of_parameter(lexit::Token tok, bool skip_to_end = false)
  {
    /* Avoid matching comma inside parameter function calls. */
    int stack = 1;
    tok = tok.next();
    while (tok.is_valid()) {
      if (tok == ParOpen) {
        stack++;
      }
      else if (tok == ParClose) {
        stack--;
      }
      if (stack == 0) {
        return tok;
      }
      if (stack == 1 && tok == Comma && !skip_to_end) {
        return tok;
      }
      tok = tok.next();
    }
    return tok;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Interface.
 * \{ */

std::string Shader::run_preprocessor(StringRef source)
{
  BLI_assert_msg(source.find("//") == std::string::npos && source.find("/*") == std::string::npos,
                 "Input source to the preprocessor should have no comments.");

  if (G.debug & G_DEBUG_GPU_SHADER_NO_PREPROCESSOR) {
    return source;
  }

  Preprocessor processor(source);
  processor.preprocess();

  if (G.debug & G_DEBUG_GPU_SHADER_NO_DCE) {
    return processor.result_get(true);
  }
  return processor.result_get(true);

  DeadCodeEliminator dce(processor.result_get(true));
  dce.optimize();
  return dce.result_get(true);
}

/** \} */

}  // namespace blender::gpu
