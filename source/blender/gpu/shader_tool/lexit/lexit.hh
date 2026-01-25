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
#include <iterator>
#include <memory>
#include <string_view>

#include "types.hh"

namespace lexit {

struct TokenBuffer {
  /* Input string. */
  std::string_view str_;
  /* Type of each token. */
  std::unique_ptr<TokenType[]> types_;
  /* Starting character index of each token. */
  std::unique_ptr<uint32_t[]> offsets_;
  /* Original character index of each next token before whitespace merging (optional). */
  std::unique_ptr<uint32_t[]> original_offsets_;
  /* Amount of tokens inside the buffer excluding the terminating EndOfFile token. */
  uint32_t size_ = 0;
  /* Amount of tokens that can be contained. */
  uint32_t allocated_size_ = 0;
  /* If whitespaces where not collapsed, offsets_ should be used instead of original_offsets_. */
  bool whitespaces_collapsed_ = false;

  TokenBuffer() = default;

  TokenBuffer(const std::string_view str, const CharClass char_class_table[128])
  {
    process(str, char_class_table);
  }

  void process(const std::string_view str, const CharClass char_class_table[128])
  {
    str_ = str;
    clear_and_reserve(str.size());
    tokenize(char_class_table);
  }

  /**
   * @brief Discard current data and allocate backing memory for the given amount of tokens.
   *
   * Does nothing if allocation is already large enough.
   */
  void clear_and_reserve(uint32_t count);

  /**
   * @brief Tokenizes the input string by grouping contiguous characters of the same class.
   *
   * This function iterates through the input string and identifies "runs" of characters
   * that map to the same CharClass. For each new group, it records the type and the
   * starting byte offset into the result arrays.
   *
   * Only characters with the #CanMerge flag are merged together.
   * Characters with class greater than #ClassToTypeThreshold will just be assigned their class as
   * #TokenType. Otherwise, the first character of the token will be used as #TokenType.
   *
   * @param char_class_table  A lookup table mapping ASCII values (0-127) to a 8-bit CharClass.
   */
  void tokenize(const CharClass char_class_table[128]);

  /**
   * @brief Merge complex literals such as floats and strings.
   */
  void merge_complex_literals();

  /**
   * @brief Merge whitespaces with their preceding token.
   */
  void merge_whitespaces();

  /**
   * @brief Return the amount of token inside the buffer.
   */
  uint32_t size() const
  {
    return size_;
  }

  struct TokenConst {
    const std::string_view str;
    const TokenType &type;
  };

  struct Token {
    const std::string_view str;
    TokenType &type;
  };

  Token operator[](int index)
  {
    int start = offsets_[index];
    int end = (whitespaces_collapsed_ ? original_offsets_ : offsets_)[index + 1];
    assert(start < end);
    return {str_.substr(start, end - start), types_[index]};
  }

  TokenConst operator[](int index) const
  {
    int start = offsets_[index];
    int end = (whitespaces_collapsed_ ? original_offsets_ : offsets_)[index + 1];
    assert(start < end);
    return {str_.substr(start, end - start), types_[index]};
  }

  /**
   * @brief Token iterator.
   */
  struct TokenIt {
    using iterator_category = std::forward_iterator_tag;

   private:
    TokenBuffer *buf_;
    int32_t index_;

   public:
    explicit TokenIt(TokenBuffer *buf, int index) : buf_(buf), index_(index) {}

    Token operator*() const
    {
      int start = buf_->offsets_[index_];
      int end = (buf_->whitespaces_collapsed_ ? buf_->original_offsets_ :
                                                buf_->offsets_)[index_ + 1];
      assert(start < end);
      return Token{buf_->str_.substr(start, end - start), buf_->types_[index_]};
    }

    TokenIt &operator++()
    {
      index_++;
      return *this;
    }

    bool operator==(const TokenIt &other) const
    {
      return index_ == other.index_;
    }
    bool operator!=(const TokenIt &other) const
    {
      return index_ != other.index_;
    }
    bool operator<(const TokenIt &other) const
    {
      return index_ < other.index_;
    }
  };

  TokenIt begin()
  {
    return TokenIt(this, 0);
  }
  TokenIt end()
  {
    return TokenIt(this, size_);
  }
};

}  // namespace lexit
