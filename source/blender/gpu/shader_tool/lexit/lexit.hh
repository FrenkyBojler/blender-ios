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

#include <cstdint>
#ifndef NDEBUG
#  include <string_view>
#endif

#include "types.hh"

namespace lexit {

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
   * @param char_class_table  A lookup table mapping ASCII values (0-127) to a 8-bit CharClass.
   */
  void tokenize(const CharClass char_class_table[128]);

  /**
   * @brief Return the amount of token inside the buffer.
   */
  uint32_t size() const
  {
    return size_;
  }

  /**
   * @brief Fuse complex literals.
   */
  void fuse_pass();

  /**
   * @brief Merge whitespaces with their preceding token.
   *
   * Cannot run before fuse_pass().
   *
   * @param original_ends  Output buffer to store the original token end offset before merging.
   *                       Must be sized to contain at least this->size()+1 amount of elements.
   */
  void merge_whitespaces(uint32_t *original_ends);

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
