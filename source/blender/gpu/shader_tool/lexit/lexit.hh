/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * LexIt is a lexer tool library focus on simplicity and efficiency.
 *
 * It is aimed at building source code processors without requiring huge dependencies like LLVM.
 * It only supports unextended-ASCII inputs that are under 4GB (because of 32bit offsets).
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <string_view>

#include "types.hh"

// #define LEXIT_DEBUG

namespace lexit {

struct TokenBuffer;

/* Unique identifier to a word token. */
using TokenAtom = uint16_t;

struct Token {
#ifdef LEXIT_DEBUG
  std::string_view debug_str_;
  TokenType debug_type_;
  TokenAtom debug_atom_;
#endif
  const TokenBuffer *buf_;
  int32_t index_;

  Token(const TokenBuffer *buf, int32_t index);

  static Token invalid(TokenBuffer *buf);

  explicit operator int32_t() const
  {
    return index_;
  }

  bool is_valid() const
  {
    return type() != EndOfFile;
  }

  TokenType type() const;
  TokenAtom atom() const;

  std::string_view str() const;
  std::string_view str_with_whitespace() const;

  bool followed_by_whitespace() const;

  Token next(int i = 1) const;
  Token prev(int i = 1) const;

  friend bool operator==(const Token &a, const Token &b)
  {
    assert(a.buf_ == b.buf_);
    return a.index_ == b.index_;
  }
  friend bool operator!=(const Token &a, const Token &b)
  {
    assert(a.buf_ == b.buf_);
    return a.index_ != b.index_;
  }

  friend bool operator==(const Token &a, TokenType b)
  {
    return a.type() == b;
  }
  friend bool operator!=(const Token &a, TokenType b)
  {
    return a.type() != b;
  }
};

/* Same as Token but allow type assignment. */
struct TokenMut : public Token {
  TokenMut(TokenBuffer *buf, int32_t index) : Token(buf, index) {}

  TokenType &type();
  TokenAtom &atom();
};

struct TokenBuffer {
  /* Input string. */
  std::string_view str_;
  /* Type of each token. */
  std::unique_ptr<TokenType[]> types_;
  /* Starting character index of each token. */
  std::unique_ptr<uint32_t[]> offsets_;
  /* Original character index of each next token before whitespace merging (optional). */
  std::unique_ptr<uint32_t[]> original_offsets_;
  /* Unique id for identifiers (Words). Externally set (optional). */
  std::unique_ptr<TokenAtom[]> atoms_;
  /* Number of tokens inside the buffer excluding the terminating EndOfFile token. */
  uint32_t size_ = 0;
  /* Number of tokens that can be contained. */
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
    clear();
    reserve(str.size());
    tokenize(char_class_table);
  }

  /**
   * @brief Discard all currently held data. Does not reallocate.
   */
  void clear();

  /**
   * @brief Allocate backing memory for the given number of tokens and move currently held data.
   *
   * Does nothing if allocation is already large enough.
   */
  void reserve(const uint32_t count);

  /**
   * @brief Tokenizes the input string by grouping contiguous characters of the same class.
   *
   * This function iterates through the input string and identifies "runs" of characters
   * that map to the same CharClass. For each new group, it records the type and the
   * starting byte offset into the result arrays.
   *
   * Only characters with the #CanMerge flag are merged together.
   * Characters with a class greater than #ClassToTypeThreshold will just be assigned their class
   * as #TokenType. Otherwise, the first character of the token will be used as #TokenType.
   *
   * @param char_class_table  A lookup table mapping ASCII values (0-127) to an 8-bit CharClass.
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
  void merge_spaces();

  /**
   * @brief Return the amount of token inside the buffer.
   */
  uint32_t size() const
  {
    return size_;
  }

  Token operator[](int index) const
  {
    return Token(this, index);
  }
  TokenMut operator[](int index)
  {
    return TokenMut(this, index);
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
    TokenIt(TokenBuffer *buf, const int index) : buf_(buf), index_(index) {}

    TokenMut operator*() const
    {
      return TokenMut(buf_, index_);
    }

    TokenIt &operator++()
    {
      index_++;
      return *this;
    }

    int32_t index() const
    {
      return index_;
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

inline Token::Token(const TokenBuffer *buf, int32_t index) : buf_(buf)
{
  assert(buf_ != nullptr);
  /* Set to Invalid / EndOfFile token if out of range. */
  index_ = (index < 0 || index > buf_->size_) ? buf_->size_ : index;
#ifdef LEXIT_DEBUG
  debug_str_ = str_with_whitespace();
  debug_type_ = type();
  debug_atom_ = atom();
#endif
}

inline Token Token::invalid(TokenBuffer *buf)
{
  return Token(buf, buf->size_);
}

inline TokenType Token::type() const
{
  return buf_->types_[index_];
}
inline TokenType &TokenMut::type()
{
  return buf_->types_[index_];
}

inline TokenAtom Token::atom() const
{
  return buf_->atoms_[index_];
}
inline TokenAtom &TokenMut::atom()
{
  return buf_->atoms_[index_];
}

inline Token Token::next(int i) const
{
  return Token(buf_, index_ + i);
}
inline Token Token::prev(int i) const
{
  return is_valid() ? Token(buf_, index_ - i) : Token(buf_, -1);
}

inline std::string_view Token::str() const
{
  int start = buf_->offsets_[index_];
  int end = buf_->whitespaces_collapsed_ ? buf_->original_offsets_[index_ + 1] :
                                           buf_->offsets_[index_ + 1];
  return {buf_->str_.data() + start, size_t(end - start)};
}

inline std::string_view Token::str_with_whitespace() const
{
  int start = buf_->offsets_[index_];
  int end = buf_->offsets_[index_ + 1];
  return {buf_->str_.data() + start, size_t(end - start)};
}

inline bool Token::followed_by_whitespace() const
{
  assert(buf_->whitespaces_collapsed_ && is_valid());
  return buf_->original_offsets_[index_ + 1] != buf_->offsets_[index_ + 1];
}

inline std::ostream &operator<<(std::ostream &os, const Token &tok)
{
  os << "Token(";
  os << "type='" << tok.type() << "', ";
  os << "atom=" << tok.atom() << ", ";
  os << "str=\"" << tok.str() << "\", ";
  os << "str_with_whitespace=\"" << tok.str_with_whitespace() << "\"";
  os << ")";
  return os;
}

}  // namespace lexit
