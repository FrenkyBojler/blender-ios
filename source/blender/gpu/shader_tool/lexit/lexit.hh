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
};

}  // namespace lexit
