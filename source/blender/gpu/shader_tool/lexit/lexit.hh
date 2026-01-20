/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * LexIt is a lexer tool library focus on simplicity and efficiency.
 *
 * It is aimed at building source code processors without requiring huge dependencies like LLVM.
 * It only supports unextended-ASCII input which are under 4GB (because of 32bit offsets).
 */

#pragma once

#include <cassert>
#include <cstdint>
#ifndef NDEBUG
#  include <string_view>
#endif

#include "types.hh"

namespace lexit {

enum class CompoundFlags : uint64_t {
  None = 0, /* Invalid. */

  Newlines = 1 << 0,   /* Merge newline with previous token. */
  Spaces = 1 << 1,     /* Merge space with previous token. */
  Strings = 1 << 2,    /* "my\"string" */
  Numbers = 1 << 3,    /* 3.e-3f */
  CompOps = 1 << 4,    /* >=, <=, ==, != */
  TokenPaste = 1 << 5, /* ## */
  LogicOps = 1 << 6,   /* ||, && */
  UnaryOps = 1 << 7,   /* ++, -- */
  Derefs = 1 << 8,     /* -> */

  Whitespaces = Newlines | Spaces, /* Merge whitespace with previous token. */
  AllButWhitespaces = String | Numbers | CompOps | TokenPaste | LogicOps | UnaryOps | Derefs,
  All = AllButWhitespaces | Whitespaces,
};

/**
 * Non-owning container for token datas stored in structure of array layout.
 */
class TokenBuffer {
 protected:
  /* Input string. */
  const uint8_t *str_;
  /* Length of the input string without including null terminator. */
  const uint32_t str_len_;
  /* Type of each token. */
  TokenType *types_;
  /* Starting character index of each token. */
  uint32_t *offsets_;
  /* Amount of tokens inside the buffer excluding the terminating EndOfFile token. */
  uint32_t size_;

 public:
  /**
   * @param c_str      An null-terminated C string.
   * @param str_len    Length of c_str excluding the null terminator.
   * @param types      An aligned array which can contain str_len+1 TokenType.
   * @param offsets    An aligned array which can contain str_len+1 uint32_t.
   * @param token_len  (optional) The amount of token already parsed.
   */
  TokenBuffer(const char *c_str,
              uint32_t str_len,
              TokenType *types,
              uint32_t *offsets,
              uint32_t token_len = 0)
      : str_((const uint8_t *)c_str),
        str_len_(str_len),
        types_(types),
        offsets_(offsets),
        size_(token_len)
  {
  }

  /**
   * @brief Tokenizes the input string by grouping contiguous characters of the same type.
   *
   * This function iterates through the input string and identifies "runs" of characters
   * that map to the same TokenType. For each new group, it records the type and the
   * starting byte offset into the result arrays.
   *
   * Only tokens with the #Merge flag are merged together.
   *
   * @param char_to_tok  A lookup table mapping ASCII values (0-127) to a 8-bit TokenType.
   */
  void tokenize(const TokenType char_to_tok[128]);

  /**
   * @brief Change each token type to Number if their first char is a number.
   *
   * This is only needed if the tokenize pass did not identify number.
   */
  void identify_numbers();

  /**
   * @brief Return the amount of token inside the buffer.
   */
  uint32_t size() const
  {
    return size_;
  }

  /**
   * @brief Merge compound tokens together.
   *
   * This is legacy and should be revisited by splitting compound (string, number) and whitespace
   * merging in two passes.
   */
  template<CompoundFlags flags> void fuse_compounds(uint32_t *rawsize = nullptr)
  {
    static_assert(flags != CompoundFlags::None, "No merge flag provided, function is a noop");

#define LEXIT_TWO(a, b) a | b << 8

    const TokenType *in_types = types_;
    TokenType *out_types = types_;
    const uint32_t *in_offsets = offsets_;
    uint32_t *out_offsets = offsets_;
    uint32_t *out_size = rawsize;

    for (uint32_t i = 0; i < size_; i++, out_types++, out_offsets++, out_size++) {
      const TokenType one = in_types[i];
      const uint16_t two = LEXIT_TWO(one, in_types[i + 1]);

      const uint32_t offset = in_offsets[i];

#ifndef NDEBUG
      std::string_view tok_str{(const char *)str_ + offset, in_offsets[i + 1] - offset};
#endif

      if (rawsize != nullptr) {
        *out_size = in_offsets[i + 1] - offset;
      }
      *out_types = one;
      *out_offsets = offset;

#define LEXIT_COMPOUND_LEX(tok, flag, ...) \
  case tok: \
    if constexpr (uint64_t(flags) & uint64_t(CompoundFlags::flag)) { \
      __VA_ARGS__; \
      continue; \
    } \
    else { \
      break; \
    }

      switch (one) {
        /* Make next token overwrite this one. Merge the token with the one before. */
        LEXIT_COMPOUND_LEX(NewLine, Newlines, if (i > 0) out_types--, out_offsets--, out_size--)
        LEXIT_COMPOUND_LEX(Space, Spaces, if (i > 0) out_types--, out_offsets--, out_size--)
        /* Complex compound call dedicated functions. */
        LEXIT_COMPOUND_LEX(String, Strings, lex_string(in_types, i))
        LEXIT_COMPOUND_LEX(Number, Numbers, lex_number(str_, in_types, in_offsets, i))

        [[likely]] default:
          break;
      }

#undef LEXIT_COMPOUND_LEX

#define LEXIT_COMPOUND_LEX(first, second, flag, new_type) \
  case LEXIT_TWO(first, second): \
    if constexpr (uint64_t(flags) & uint64_t(CompoundFlags::flag)) { \
      *out_types = new_type; \
      break; \
    } \
    else { \
      continue; \
    }

      switch (two) {
        LEXIT_COMPOUND_LEX('=', '=', CompOps, Equal)
        LEXIT_COMPOUND_LEX('!', '=', CompOps, NotEqual)
        LEXIT_COMPOUND_LEX('<', '=', CompOps, GEqual)
        LEXIT_COMPOUND_LEX('>', '=', CompOps, LEqual)
        LEXIT_COMPOUND_LEX('#', '#', TokenPaste, DoubleHash)
        LEXIT_COMPOUND_LEX('&', '&', LogicOps, LogicalAnd)
        LEXIT_COMPOUND_LEX('|', '|', LogicOps, LogicalOr)
        LEXIT_COMPOUND_LEX('+', '+', UnaryOps, Increment)
        LEXIT_COMPOUND_LEX('-', '-', UnaryOps, Decrement)
        LEXIT_COMPOUND_LEX('-', '>', Derefs, Deref)
        [[likely]] default:
          continue;
      }
#undef LEXIT_COMPOUND_LEX

      i++; /* Skip next token. */
    }
#undef LEXIT_TWO

    assert(in_types < out_types);
    assert(out_types - in_types < 0xFFFFFFFFu);
    size_ = out_types - in_types;
    types_[size_] = EndOfFile;
    offsets_[size_] = str_len_;
    if (rawsize) {
      rawsize[size_] = 0;
    }
  }

  template<typename CallbackFn> void foreach_token_type(CallbackFn cb)
  {
    TokenType *end = types_ + size_;
    for (TokenType *type = types_; type < end; type++) {
      cb(type);
    }
  }

 private:
  void lex_string(const TokenType *types, uint32_t &cursor);

  void lex_number(const uint8_t c_str[/*size*/],
                  const TokenType types[/*tok_len*/],
                  const uint32_t offsets[/*tok_len*/],
                  uint32_t &cursor);
};

}  // namespace lexit
