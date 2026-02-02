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

using LexerBase = shader::parser::LexerBase;
using NullParser = shader::parser::NullParser;
using namespace lexit;

template<typename IToken> struct TokenRange {
  IToken begin, end;
};

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

  /* Chosen to be easily masked. */
  constexpr static TokenAtom long_atom_range_start = 0x8000;

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

  std::deque<std::string> pasted_token;

  struct PasteResult {
    TokenAtom atom;
    int paste_tok_index;
  };

  BLI_INLINE_METHOD PasteResult paste_token(const std::string &tok_str)
  {
    int index = pasted_token.size();
    pasted_token.emplace_back(tok_str);
    /** IMPORTANT: The hash function need to store a StringRef of the string. We have to make sure
     * to feed it the final stored string to avoid referencing freed memory. */
    TokenAtom atom = hash(pasted_token.back());
    return {atom, index};
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
      case perfect_hash("pragma"):
        return (s == "pragma") ? Pragma : Word;
      default:
        return Word;
    }
  }

  /** Map string hashes to atom value. */
  Map<StringRef, TokenAtom> atomization_map_;
  /* Reserve top range for longer token. */
  uint16_t atom_hash_counter_ = long_atom_range_start;

  uint16_t next_hash()
  {
    /* Check for overflow. */
    BLI_assert(atom_hash_counter_ >= long_atom_range_start);
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

    std::memset(atoms_.get(), 0, token_types.size() * sizeof(TokenAtom));

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

struct Stream {
  struct Space {};
  struct True {};
  struct False {};
  struct ConcatNext {};

  struct SToken {
    /* -1 if boolean value. */
    int32_t index = -1;
    /* Non zero if token is pasted and should be sourced from the pasted token buffer. */
    TokenAtom pasted_atom = 0;
    /* True if token is followed by whitespace or a space was injected after it. */
    bool followed_by_space = false;
    /* Value of the token if True/False from expression expansion. */
    bool bool_value;

    SToken(Token tok) : index(int(tok)), followed_by_space(tok.followed_by_whitespace()) {}
    SToken(True /*tok*/) : bool_value(true) {}
    SToken(False /*tok*/) : bool_value(false) {}

    /* For pasted tokens. */
    SToken(int index, TokenAtom atom, bool followed_by_space)
        : index(index), pasted_atom(atom), followed_by_space(followed_by_space)
    {
      BLI_assert(index != -1);
    }
  };

  AtomicLexer &lex;

  bool concat_next = false;

  Vector<SToken> tokens;

  Stream(AtomicLexer &lex) : lex(lex) {};

  void clear()
  {
    tokens.clear();
  }

  Stream &operator<<(const Stream &stream)
  {
    tokens.extend(stream.tokens);
    return *this;
  }

  Stream &operator<<(const Token tok)
  {
    if (UNLIKELY(concat_next)) {
      tokens.last() = paste_token(tok.str(), tok.followed_by_whitespace());
      concat_next = false;
    }
    else {
      tokens.append(tok);
    }
    return *this;
  }
  /* NOTE: Not compatible with concatenation. */
  Stream &operator<<(True tok)
  {
    tokens.append(tok);
    return *this;
  }
  /* NOTE: Not compatible with concatenation. */
  Stream &operator<<(False tok)
  {
    tokens.append(tok);
    return *this;
  }

  template<typename IToken> Stream &operator<<(const TokenRange<IToken> &stream)
  {
    for (IToken tok = stream.begin; tok != stream.end; tok = tok.next()) {
      *this << tok;
    }
    /* "Cancel" concatenation in case range is empty. */
    concat_next = false;
    return *this;
  }

  Stream &operator<<(const ConcatNext /*concat*/)
  {
    /* Don't concat if there is nothing to concatenate. */
    concat_next = !tokens.is_empty();
    return *this;
  }

  Stream &operator<<(Space /*space*/)
  {
    if (!tokens.is_empty()) {
      tokens.last().followed_by_space = true;
    }
    return *this;
  }

  std::string result_buf;

  /* TODO(fclem): Remove this. Only there for expansion parser. */
  std::string str()
  {
    result_buf.clear();
    result_buf.reserve(tokens.size() * 7);
    for (const SToken stream_tok : tokens) {
      if (UNLIKELY(stream_tok.index == -1)) {
        result_buf += char('0' + stream_tok.bool_value);
      }
      else if (UNLIKELY(stream_tok.pasted_atom)) {
        result_buf += lex.pasted_token[stream_tok.index];
      }
      else {
        result_buf += lex[stream_tok.index].str();
      }
      if (stream_tok.followed_by_space) {
        result_buf += ' ';
      }
    }
    return result_buf;
  }

  struct IToken {
    const AtomicLexer *lex;
    const SToken *stream_tok;

    IToken(const AtomicLexer &lex, const SToken *stream_tok) : lex(&lex), stream_tok(stream_tok) {}
    IToken(const IToken &other) = default;

    bool is_valid() const
    {
      return true; /* TODO */
    }

    TokenType type() const
    {
      BLI_assert_msg(stream_tok->index != -1, "Cannot iterate a stream with boolean tokens");
      return stream_tok->pasted_atom ? Word : (*lex)[stream_tok->index].type();
    }
    TokenAtom atom() const
    {
      BLI_assert_msg(stream_tok->index != -1, "Cannot iterate a stream with boolean tokens");
      return stream_tok->pasted_atom ? stream_tok->pasted_atom : (*lex)[stream_tok->index].atom();
    }

    std::string_view str() const
    {
      BLI_assert_msg(stream_tok->index != -1, "Cannot iterate a stream with boolean tokens");
      return stream_tok->pasted_atom ? std::string_view{lex->pasted_token[stream_tok->index]} :
                                       (*lex)[stream_tok->index].str();
    }

    bool followed_by_whitespace() const
    {
      BLI_assert_msg(stream_tok->index != -1, "Cannot iterate a stream with boolean tokens");
      return stream_tok->pasted_atom ? stream_tok->followed_by_space :
                                       (*lex)[stream_tok->index].followed_by_whitespace();
    }

    IToken next() const
    {
      /*TODO Safety*/
      return IToken(*lex, stream_tok + 1);
    }
    IToken prev() const
    {
      /*TODO Safety*/
      return IToken(*lex, stream_tok - 1);
    }

    friend bool operator==(const IToken &a, const IToken &b)
    {
      return a.stream_tok == b.stream_tok;
    }
    friend bool operator!=(const IToken &a, const IToken &b)
    {
      return a.stream_tok != b.stream_tok;
    }

    friend bool operator==(const IToken &a, TokenType b)
    {
      return a.type() == b;
    }
    friend bool operator!=(const IToken &a, TokenType b)
    {
      return a.type() != b;
    }

    /* For iterator compatibility. */
    IToken &operator++()
    {
      stream_tok++;
      return *this;
    }
    const IToken &operator*()
    {
      return *this;
    }
  };

  Stream &operator<<(const IToken &tok)
  {
    if (UNLIKELY(concat_next)) {
      tokens.last() = paste_token(tok.str(), tok.followed_by_whitespace());
      concat_next = false;
    }
    else {
      tokens.append(*tok.stream_tok);
    }
    return *this;
  }

  IToken begin() const
  {
    return IToken{lex, tokens.begin()};
  }

  IToken end() const
  {
    return IToken{lex, tokens.end()};
  }

 private:
  SToken paste_token(const StringRef &b, bool followed_by_space)
  {
    std::string pasted = IToken{lex, &tokens.last()}.str() + b;
    auto [atom, id] = lex.paste_token(pasted);
    return SToken(id, atom, followed_by_space);
  }
};

DCEStream &operator<<(DCEStream &dst, const Stream &src)
{
  for (const auto stream_tok : src.tokens) {
    /* This should only be used for expressions. */
    BLI_assert(stream_tok.index != -1);

    if (UNLIKELY(stream_tok.pasted_atom)) {
      /* TODO: Pasting could eventually form a keyword. But that's not supported yet. */
      dst.parse_token(stream_tok.pasted_atom, Word);
      dst << src.lex.pasted_token[stream_tok.index];
    }
    else {
      Token tok = src.lex[stream_tok.index];
      dst.parse_token(tok.atom(), tok.type());
      dst << tok.str();
    }

    if (stream_tok.followed_by_space) {
      dst << " ";
    }
  }
  return dst;
}

DCEStream &operator<<(DCEStream &dst, const TokenRange<Token> &range)
{
  dst.parse_token(range.begin, range.end);
  StringRef str = range.begin.str_with_whitespace();
  dst << StringRef(str.data(), range.end.str_with_whitespace().end());
  return dst;
}

DCEStream &operator<<(DCEStream &dst, const Token &tok)
{
  dst.parse_token(tok.atom(), tok.type());
  dst << tok.str_with_whitespace();
  return dst;
}

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

shader::parser::report_callback report_fn_ptr = report_fn;

/**
 * Boiler plate class exposing lexer structure using typed IDs.
 */
struct IntermediateFormWithIDs : shader::parser::IntermediateForm<AtomicLexer, NullParser> {

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
    Pragma = TokenType::Pragma,
    /* Any other unhandled directives (warnings / errors / pragma etc...). */
    Other = TokenType::Word,
  };

  /* Cached keyword identifier. */
  AtomID defined_atom = get_atom("defined");
  AtomID va_args_atom = get_atom("__VA_ARGS__");

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
    Token tok_start = lex_[int(get_start(start))];
    Token tok_end = lex_[int(get_end(end))];
    return substr_range_inclusive_view(tok_start, tok_end);
  }
  StringRef str(LineID line)
  {
    Token start = get_start(line);
    Token end = get_end(line);
    return substr_range_inclusive_view(start, end);
  }

  StringRef str_with_whitespace(DirectiveID dir)
  {
    LineID start = get_start(dir);
    LineID end = get_end(dir);
    Token tok_start = lex_[int(get_start(start))];
    Token tok_end = lex_[int(get_true_end(end))];
    return substr_range_inclusive_view(tok_start, tok_end);
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
  Token get_start(LineID line)
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
  Token get_end(LineID line)
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
  Token get_true_end(LineID line)
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
  Token get_identifier(DirectiveID dir)
  {
    LineID line = get_start(dir);
    Token hash_tok = get_start(line);
    BLI_assert(hash_tok == Hash);
    Token dir_tok = hash_tok.next();
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
  using ExpressionLexer = shader::parser::ExpressionLexer;
  using ExpressionParser = shader::parser::ExpressionParser;
  using ExpansionParser = IntermediateForm<ExpansionLexer, shader::parser::DummyParser>;

  DCEStream out_stream;

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

  struct Macro {
    DirectiveID id;
    StreamPtr definition;
    Vector<AtomID> params;
    bool is_function = false;
    bool contains_concat = false;

    Macro(DirectiveID id) : id(id) {}

    bool is_empty() const
    {
      return definition->tokens.is_empty();
    }
  };

  /* When evaluating a condition directive inside this stack, disregard the directive and jump to
   * the matching #endif. */
  Vector<DirectiveID, 8> jump_stack;
  /* Own stack to avoid memory allocation during recursive expansion parsing. */
  StreamPool stream_pool = {lex_};
  /* Set of visited macros during recursion (blue painting stack). Using a vector for speed. */
  Vector<DirectiveID> visited_macros;

  /* Maps containing currently active macros. Map their keyword to their definition. */
  Map<AtomID, DirectiveID> defines;
  /* Cached parsed data for each Macro. Allow lazy parsing. Indexed by DirectiveID. */
  Vector<std::unique_ptr<Macro>> parsed_macro;
  /* Cache allowing fast conservative rejection of tokens not matching any declared macro. */
  struct MacroBucketCache {
    /**
     * Cached buckets of enabled macros atoms.
     * Each bit represent a bucket (of variable size).                                Bucket Size
     * Bits 0-62:    Single char identifiers           (atom < 128)                        1 atom
     * Bits 64-127:  Double char identifiers           (atom < long_atom_range_start)    128 atoms
     * Bits 128-511: Long identifiers                  (atom >= long_atom_range_start)     8 atoms
     * Bits 63:      Long identifiers over cache limit (atom > 35840)                  29695 atoms
     *
     * This atom distribution is based on our source code statistics.
     * It is rare to have double char identifiers and even more rare to have macros defined for
     * them. It is also unlikely to have more than 3072 unique identifiers inside a compilation
     * unit. Our upper-bound is around 2500.
     *
     * When a macro is defined, the bucket the identifier belongs to is set occupied.
     *
     * This table is made to fit one cache line.
     */
    alignas(64) uint64_t enabled_macro_buckets[8] = {0};

    /* Set the overflow atom to an invalid atom value for an identifier (DEL). */
    static constexpr TokenAtom overflow_atom = 0x7F;

    /* Check if the bucket associated with the atom was marked as occupied. */
    bool is_occupied(AtomID atom)
    {
      auto [bin, bit] = bucket_lookup(TokenAtom(atom));
      return ((enabled_macro_buckets[bin] >> bit) & 1) != 0;
    }

    /* Mark the bucket associated with the atom as occupied. */
    void set_occupied(AtomID atom)
    {
      auto [bin, bit] = bucket_lookup(TokenAtom(atom));
      enabled_macro_buckets[bin] |= 1ull << bit;
    }

   private:
    struct BucketResult {
      uint8_t bin;
      uint8_t bit;
    };

    BucketResult bucket_lookup(TokenAtom atom)
    {
      /* Exceed the cache limit. */
      const bool overflow = (atom >= AtomicLexer::long_atom_range_start + 6 * 64 * 8);
      /* Set overflow to a reserved value. */
      atom = overflow ? overflow_atom : atom;
      /* Two char identifier. Almost never used. Use second bitmask */
      const bool short_identifier = (atom >= 128);
      /* Long identifier. Use last 6 bitmasks. */
      const bool long_identifier = (atom >= AtomicLexer::long_atom_range_start);
      /* One char identifier use first bitmask. */
      uint8_t bin = 0;
      /* Two char identifier. Almost never used. Use second bitmask */
      bin += short_identifier;
      /* Long identifier bitmask start at 2.*/
      bin += long_identifier;
      /* Long identifier bitmask. Each bitmask covers 64 * 8 values. */
      bin += (atom / (64u * 8u)) & (7u * long_identifier);
      /* One char identifier use low 6 bits. */
      uint8_t bit = (atom >> (long_identifier * 3)) & 63u;
      return {bin, bit};
    }
  } macro_buckets;

  /**
   * State Tracking.
   */

  /* Next preprocessor directive to evaluate. Might be overwritten by conditional evaluation. */
  DirectiveID next_directive = DirectiveID::invalid();
  /* End of the last evaluated directive. Might be overwritten by conditional evaluation.
   * Used to resume token expansion after this line. */
  LineID last_directive_end = LineID::invalid();

 public:
  Preprocessor(const std::string_view str)
      : IntermediateFormWithIDs(str),
        out_stream(
            lex_.hash("return"), lex_.hash("thread"), lex_.hash("device"), lex_.hash("layout"))
  {
    /* From our stats. Should be enough for 100% of our cases. */
    defines.reserve(1000);
    /* Ensure one slot for each directive. */
    parsed_macro.resize(lex_.directive_lines.size());
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

  void optimize()
  {
    Vector<TokenAtom> entry_points;
    entry_points.append(lex_.hash("main"));
    /* TODO(fclem): Properly support forward declaration. */
    entry_points.append(lex_.hash("nodetree_displacement"));
    entry_points.append(lex_.hash("nodetree_surface"));
    entry_points.append(lex_.hash("nodetree_volume"));
    entry_points.append(lex_.hash("nodetree_thickness"));
    entry_points.append(lex_.hash("derivative_scale_get"));
    entry_points.append(lex_.hash("closure_to_rgba"));

    out_stream.optimize(entry_points.as_span());
  }

  std::string result_get()
  {
    return out_stream.str();
  }

 private:
  void evaluate_directive(DirectiveID dir)
  {
    DirectiveType dir_type = get_type(dir);

    /* Note: gets overwritten by conditional processing. */
    last_directive_end = get_end(dir);

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
        break;
      case Line:
        erase_lines(get_start(dir), get_end(dir));
        break;
      case Endif:
        erase_lines(get_start(dir), get_end(dir));
        break;
      case Pragma:
        process_pragma(dir);
        ATTR_FALLTHROUGH;
      case Other:
        out_stream << str_with_whitespace(dir);
        break;
    }
  }

  /**
   * Pragmas.
   */

  TokenAtom blender_atom = lex_.hash("blender");
  TokenAtom dce_atom = lex_.hash("dead_code_elimination");
  TokenAtom off_atom = lex_.hash("off");
  TokenAtom on_atom = lex_.hash("on");

  BLI_NOINLINE void process_pragma(DirectiveID dir)
  {
    Token tok = get_identifier(dir).next();
    if (tok.atom() == blender_atom) {
      tok = tok.next();
      if (tok.atom() == dce_atom) {
        tok = tok.next();
        if (tok.atom() == off_atom) {
          out_stream.set_enabled_parsing(false);
        }
        else if (tok.atom() == on_atom) {
          out_stream.set_enabled_parsing(true);
        }
        else {
          BLI_assert_msg(false, "Invalid dead_code_elimination pragma. Expecting on or off.");
        }
      }
    }
  }

  /**
   * Macro Management.
   */

  /**
   * Parse macro parameters and advance the token cursor.
   */
  BLI_NOINLINE Vector<AtomID> parse_macro_params(Token &tok)
  {
    Vector<AtomID> macro_parameters;
    while (true) {
      /* Continue to the next name. */
      tok = tok.next();
      if (tok == Backslash && tok.next() == NewLine) {
        /* Preprocessor new line. Skip and continue. */
        tok = tok.next(2);
        continue;
      }
      if (tok == ParClose) {
        break;
      }
      macro_parameters.append(tok.str() == "..." ? va_args_atom : AtomID(tok.atom()));
      /* Continue to the next separator. */
      tok = tok.next();
      if (tok == Backslash && tok.next() == NewLine) {
        /* Preprocessor new line. Skip and continue. */
        tok = tok.next(2);
        continue;
      }
      if (tok == ParClose) {
        break;
      }
    }
    /* Skip closing parenthesis. */
    tok = tok.next();

    return macro_parameters;
  }

  BLI_NOINLINE StreamPtr parse_macro_definition(Token definition_start_tok, bool &contains_concat)
  {
    StreamPtr definition = stream_pool.alloc();

    Token tok = definition_start_tok;
    while (true) {
      Token tok_next = tok.next();
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
        contains_concat = true;
        tok = tok.next(2);
        continue;
      }
      BLI_assert_msg(tok != Hash, "Stringify operator is not supported");
      *definition << tok;
      tok = tok_next;
    }
    return definition;
  }

  BLI_NOINLINE void define_macro(DirectiveID dir)
  {
    const Token name = get_identifier(dir).next();
    BLI_assert(name == Word);
    defines.add_overwrite(AtomID(name.atom()), dir);
    macro_buckets.set_occupied(AtomID(name.atom()));
    erase_lines(get_start(dir), get_end(dir));
  }

  BLI_NOINLINE Macro &get_macro(DirectiveID dir)
  {
    if (parsed_macro[int(dir)] == nullptr) {
      const Token name = get_identifier(dir).next();
      const Token after_name = name.next();
      Token cursor = after_name;

      auto macro = std::make_unique<Macro>(dir);
      macro->is_function = (after_name == ParOpen) && !name.followed_by_whitespace();
      if (macro->is_function) {
        macro->params = parse_macro_params(cursor);
      }
      macro->definition = parse_macro_definition(cursor, macro->contains_concat);

      parsed_macro[int(dir)] = std::move(macro);
    }
    return *parsed_macro[int(dir)];
  }

  void undefine_macro(DirectiveID dir)
  {
    const Token name = get_identifier(dir).next();
    BLI_assert(name == Word);
    defines.remove(AtomID(name.atom()));
    erase_lines(get_start(dir), get_end(dir));
  }

  /**
   * Condition directives.
   */

  BLI_NOINLINE void process_conditional(const DirectiveID dir, const DirectiveType dir_type)
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
    const Token dir_tok = get_identifier(dir);
    /* Evaluate condition. */
    const Token cond_start = dir_tok.next();
    const Token cond_end = get_end(dir_line_end);
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

  bool evaluate_condition(const DirectiveType dir_type, Token start, Token end)
  {
    switch (dir_type) {
      case Else:
        return true;
      case Ifdef:
        return defines.contains(AtomID(start.atom()));
      case Ifndef:
        return !defines.contains(AtomID(start.atom()));
      case If:
      case Elif:
        return evaluate_expression(start, end);
      default:
        BLI_assert_unreachable();
        return true;
    }
  }

  bool evaluate_expression(const Token start, const Token end)
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
    if (expand->tokens.size() == 1) {
      const auto &token = expand->tokens.first();
      if (token.index == -1) {
        return token.bool_value;
      }
    }

    try {
      /* TODO(fclem): Do not parse again. Simply use the token stream. */
      std::string str = expand->str();
      expression_lexer.lexical_analysis(str);
      return expression_parser.eval() != 0;
    }
    catch (const std::exception &e) {
      std::cout << "\"" << substr_range_inclusive_view(start, end) << "\" > \"" << expand->str()
                << "\" ";
      std::cerr << "Error: " << e.what() << "\n";
      return false;
    }
  }

  /**
   * Macro Expansion.
   */

  BLI_NOINLINE void expand_macros_in_range(const LineID start_line, const LineID end_line)
  {
    int start = int(get_start(start_line));
    int end = int(get_true_end(end_line));
    if (start > end) {
      return;
    }
    if (start == end) {
      out_stream << lex_[start];
      return;
    }

    const Vector<int> &candidates = gather_candidate_in_range(start, end);

    Token after_last_emitted = lex_[start];

    for (const auto *it = candidates.begin(); it != candidates.end();) {
      const Token tok = lex_[*it];
      const DirectiveID macro_id = defines.lookup_default(AtomID(tok.atom()),
                                                          DirectiveID::invalid());
      /* Emit tokens between the last emitted token and this one. */
      if (int(after_last_emitted) < *it) {
        out_stream << TokenRange<Token>{after_last_emitted, tok.prev()};
      }

      if (macro_id == DirectiveID::invalid()) {
        out_stream << tok;
        after_last_emitted = tok.next();
        ++it;
        continue;
      }

      const Token end_tok = expand_and_replace(tok, macro_id);
      after_last_emitted = end_tok.next();
      /* Skip candidates already expanded. */
      while (it != candidates.end() && *it <= int(end_tok)) {
        ++it;
      }
    }

    const Token last_tok = lex_[end];
    out_stream << TokenRange<Token>{after_last_emitted, last_tok};
  }

  /* Cached vector to avoid reallocation. */
  Vector<int> expand_candidates_;

  /* Perform coarse check using small cache table of defined macros.
   * Returns a vector of token index that can potentially expand.
   * This avoid costly hash table lookups for every token. */
  BLI_NOINLINE const Vector<int> &gather_candidate_in_range(int start, int end)
  {
    expand_candidates_.clear();
    expand_candidates_.resize(end - start + 1);
    int candidates_count = 0;
    /* TODO(fclem): This is the major bottleneck after the actual expansion.
     * Can be sped up using SIMD. */
    for (int i : blender::IndexRange::from_begin_end(start, end)) {
      /* Branchless insertion. Faster than. */
      expand_candidates_[candidates_count] = i;
      candidates_count += macro_buckets.is_occupied(AtomID(lex_.atoms_[i]));
    }
    expand_candidates_.resize(candidates_count);
    return expand_candidates_;
  }

  BLI_NOINLINE Token expand_and_replace(const Token tok, const DirectiveID macro_id)
  {
    auto [replacement, end] = expand_macro(tok, get_macro(macro_id));
    out_stream << *replacement;
    return end;
  }

  /* Parse and expand with the current set of macro identifier. */
  BLI_NOINLINE StreamPtr parse_and_expand(const Stream &tok_stream)
  {
    auto result = stream_pool.alloc();

    for (auto tok = tok_stream.begin(), end = tok_stream.end(); tok != end; tok = tok.next()) {
      if (tok == Word) {
        /* Try to match the token pointed at by cursor with a defined macro. If that happen advance
         * the cursor to the end of the macro (in case of functional macro). */
        DirectiveID macro_id = defines.lookup_default(AtomID(tok.atom()), DirectiveID::invalid());
        if (macro_id != DirectiveID::invalid()) {
          auto [replacement, end] = expand_macro(tok, get_macro(macro_id));
          *result << *replacement;
          tok = end;
          continue;
        }
      }
      *result << tok;
    }
    return result;
  }

  template<typename IToken> struct ExpandedResult {
    /* Replacement content. */
    StreamPtr output;
    /* End of range to replace. */
    IToken end_of_expansion;
  };

  /**
   * \arg tok : Opening parenthesis token of the argument list.
   */
  template<typename IToken>
  BLI_NOINLINE Vector<TokenRange<IToken>> parse_macro_args(const Vector<AtomID> &params,
                                                           IToken &tok)
  {
    Vector<TokenRange<IToken>> args;
    for (const AtomID param_atom : params) {
      IToken param_start = tok;
      IToken param_end = get_end_of_parameter(param_start);

      if (param_atom == va_args_atom) {
        /* Seek end of argument list for variadic arguments. */
        param_end = get_end_of_parameter(param_start, true);
      }

      args.append({param_start.next(), param_end});
      /* Continue to the next separator. */
      tok = param_end;
      if (tok == Invalid) {
        break;
      }
    }
    /* No parameter case. */
    if (params.is_empty()) {
      /* Continue to end parenthesis. */
      tok = tok.next();
    }
    if (tok == Invalid) {
      /* Error: missing closing parenthesis. */
      /* Cancel expansion. */
      return {};
    }
    if (tok != ParClose) {
      /* Error: too many arguments provided to function-like macro invocation. */
      /* Cancel expansion. */
      return {};
    }
    return args;
  }

  /* TODO(fclem): Try specialization for no concatenation and no function. */
  template<typename IToken>
  BLI_NOINLINE StreamPtr expand_macro_args(const Macro &macro,
                                           const Vector<TokenRange<IToken>> &macro_args)
  {
    StreamPtr ts = stream_pool.alloc();
    for (const auto &def_tok : *macro.definition) {
      if (def_tok.type() == Hash) {
        *ts << Stream::ConcatNext{};
        continue;
      }
      if (def_tok.type() == Word && !macro_args.is_empty()) {
        /* Lookup macro arguments. */
        int arg_id = macro.params.first_index_of_try(AtomID(def_tok.atom()));
        if (arg_id != -1) {
          const TokenRange<IToken> &macro_value = macro_args[arg_id];

          if (def_tok.prev() == Hash || def_tok.next() == Hash) {
            /* Token pasting. Do not expand now. */
            *ts << macro_value;
          }
          else {
            /* Expand argument. Can expand to the same macro (finite recursion). */
            StreamPtr stream = stream_pool.alloc();
            *stream << macro_value;
            *ts << *parse_and_expand(*stream);
          }

          if (def_tok.followed_by_whitespace()) {
            /* Don't lose whitespace after macro token. */
            *ts << Stream::Space{};
          }
          continue;
        }
      }
      *ts << def_tok;
    }
    return ts;
  }

  template<typename IToken>
  BLI_NOINLINE ExpandedResult<IToken> expand_macro(const IToken expanded_tok, const Macro &macro)
  {
    if (visited_macros.contains(macro.id)) {
      /* Recursion. Do not expand. */
      /* Currently still replace by the original token (noop).
       * Would be better to not bypass replacement alltogether. */
      StreamPtr expanded = stream_pool.alloc();
      *expanded << expanded_tok;
      return {std::move(expanded), expanded_tok};
    }

    IToken end_of_expansion = expanded_tok;

    Vector<TokenRange<IToken>> fn_arguments;
    if (macro.is_function) {
      IToken tok = expanded_tok;
      tok = tok.next();
      if (tok != ParOpen) {
        /* Macro doesn't have parameters. It should not expand. */
        /* Currently still replace by the original token (noop).
         * Would be better to bypass replacement alltogether. */
        StreamPtr expanded = stream_pool.alloc();
        *expanded << expanded_tok;
        return {std::move(expanded), expanded_tok};
      }

      fn_arguments = parse_macro_args(macro.params, tok);
      /* Make sure to replace the whole call. */
      end_of_expansion = tok;
    }

    if (macro.is_empty()) {
      /* Empty definition. */
      StreamPtr expanded = stream_pool.alloc();
      return {std::move(expanded), end_of_expansion};
    }

    StreamPtr result;

    if (!macro.is_function && !macro.contains_concat) {
      /* Fast Path. */
      visited_macros.append(macro.id);
      result = parse_and_expand(*macro.definition);
      visited_macros.pop_last();
    }
    else {
      StreamPtr expanded = expand_macro_args(macro, fn_arguments);
      /* Add to the set to avoid infinite recursion. */
      visited_macros.append(macro.id);
      result = parse_and_expand(*expanded);
      visited_macros.pop_last();
    }

    if (end_of_expansion.followed_by_whitespace()) {
      *result << Stream::Space{};
    }

    return {std::move(result), end_of_expansion};
  }

  /* Expand token range for condition evaluation (e.g. '#if'). */
  StreamPtr expand_expression(const Token start, const Token end)
  {
    StreamPtr result = stream_pool.alloc();

    Token tok = start;
    while (true) {
      BLI_assert(tok.is_valid());
      AtomID tok_atom = tok == Word ? AtomID(tok.atom()) : AtomID::invalid();

      DirectiveID macro_id = defines.lookup_default(AtomID(tok.atom()), DirectiveID::invalid());

      if (tok_atom == AtomID::invalid()) {
        /* Non word. */
        *result << lex_[int(tok)];
      }
      else if (macro_id != DirectiveID::invalid()) {
        Macro &macro = get_macro(macro_id);
        auto [replacement, macro_end] = expand_macro(Token(lex_[int(tok)]), macro);
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
        if (defines.contains(AtomID(tok.atom()))) {
          *result << Stream::True{};
        }
        else {
          *result << Stream::False{};
        }
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
    if (int(end) > int(start)) {
      out_stream << new_lines(start, end);
    }
    out_stream << get_true_end(end).str_with_whitespace();
  }

  /* Return a string with the amount of newline character between line_start and line_end. */
  StringRef new_lines(LineID line_start, LineID line_end)
  {
    lex_.pasted_token.emplace_back(int(line_end) - int(line_start), '\n');
    return lex_.pasted_token.back();
  }

  Token skip_directive_newlines(Token tok)
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
  template<typename IToken>
  static IToken get_end_of_parameter(IToken tok, bool skip_to_end = false)
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
    return processor.result_get();
  }
  processor.optimize();
  return processor.result_get();
}

/** \} */

}  // namespace blender::gpu
