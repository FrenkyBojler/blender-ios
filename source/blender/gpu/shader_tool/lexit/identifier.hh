/* SPDX-FileCopyrightText: 2026 Clement Foucault
 *
 * SPDX-License-Identifier: MIT */

#pragma once

#include <cstdint>
#include <string_view>

#include "types.hh"
#include "vector.hh"

#if defined(_MSC_VER)
#  define INLINE_METHOD __forceinline
#else
#  define INLINE_METHOD inline __attribute__((always_inline))
#endif

namespace lexit {

static constexpr uint64_t padded_string_masks[8] = {
    uint64_t(0xFFFFFFFFFFFFFFFF),
    uint64_t(0x00000000000000FF),
    uint64_t(0x000000000000FFFF),
    uint64_t(0x0000000000FFFFFF),
    uint64_t(0x00000000FFFFFFFF),
    uint64_t(0x000000FFFFFFFFFF),
    uint64_t(0x0000FFFFFFFFFFFF),
    uint64_t(0x00FFFFFFFFFFFFFF),
};

/* Copy of a small string onto aligned bytes. This avoids the cost of calling memcmp. */
struct PaddedString16 {
  uint64_t data[2] = {0, 0};

  PaddedString16() = default;
  PaddedString16(std::string_view str)
  {
    // assert(str.size() <= 16);
    /* In order for this to not be slow, we need to copy a known quantity. This is why the caller
     * needs to ensure the source extends enough bytes after the start `str`. */
    std::memcpy(data, (const char *)str.data(), sizeof(data));
    /* Fast way of masking the excess chars. */
    int last_qword = (str.size() - 1) >> 3;
    data[last_qword] &= padded_string_masks[str.size() & 7];
  }

  friend bool operator==(const PaddedString16 &, const PaddedString16 &) = default;
};

struct PaddedString8 {
  uint64_t data = 0;

  PaddedString8() = default;
  /* Source is already padded. */
  constexpr PaddedString8(const uint64_t str) : data(str) {}
  /* Note that this looses the size requirement. To be used with caution. */
  explicit constexpr PaddedString8(const PaddedString16 &s16) : data(s16.data[0]) {}

  constexpr PaddedString8(char c0, char c1, char c2, char c3, char c4, char c5, char c6, char c7)
      : data((uint64_t(c0) << 0) | (uint64_t(c1) << 8) | (uint64_t(c2) << 16) |
             (uint64_t(c3) << 24) | (uint64_t(c4) << 32) | (uint64_t(c5) << 40) |
             (uint64_t(c6) << 48) | (uint64_t(c7) << 56))
  {
  }

  PaddedString8(std::string_view str)
  {
    // assert(str.size() <= 8);
    /* In order for this to not be slow, we need to copy a known quantity. This is why the caller
     * needs to ensure the source extends enough bytes after the start `str`. */
    std::memcpy(&data, (const char *)str.data(), sizeof(data));
    /* Fast way of masking the excess chars. */
    data &= padded_string_masks[str.size() & 7];
  }

  friend bool operator==(const PaddedString8 &, const PaddedString8 &) = default;
};

struct IdentifierMap {
  struct alignas(8) Identifier {
    uint16_t next;
    uint16_t size;
    uint32_t hash;
    uint64_t data[0];

    /* Caller must ensure size matches. */
    bool operator==(PaddedString16 str) const
    {
      if (size > 8) {
        return data[0] == str.data[0] && data[1] == str.data[1];
      }
      return data[0] == str.data[0];
    }

    bool operator==(std::string_view str) const
    {
      return std::string_view{(const char *)data, size} == str;
    }
  };

  lexit::Vector<Identifier> identifier_buffer;

  /* Note: Must be power of two size. */
  static constexpr uint32_t hash_table_size = 16384;
  static constexpr uint32_t hash_table_index_mask = (hash_table_size - 1);
  std::array<uint16_t, hash_table_size> hash_table;

  IdentifierMap()
  {
    /* Set invalid values for all the table. */
    std::memset(hash_table.data(), 0xFFu, sizeof(uint16_t) * hash_table.size());
  }

  void reserve(int token_count)
  {
    identifier_buffer.reserve(token_count);
  }

  /* Return the maximum value for the currently allocated atoms. */
  TokenAtom max_atom_value() const
  {
    return identifier_buffer.size();
  }

  static constexpr uint32_t str_hash(std::string_view s)
  {
    uint32_t hash = 5381;
    hash = ((hash << 5) + hash) + s.size();
    hash = ((hash << 5) + hash) + static_cast<uint8_t>(s[0]);
    hash = ((hash << 5) + hash) + static_cast<uint8_t>(s[s.size() / 2]);
    hash = ((hash << 5) + hash) + static_cast<uint8_t>(s.back());
    return static_cast<uint16_t>(hash);
  }

  INLINE_METHOD TokenAtom lookup_or_add(PaddedString16 str, size_t str_size)
  {
    std::string_view str_view{(const char *)&str, str_size};
    uint32_t hash = str_hash(str_view);
    uint16_t index = hash_table[hash & hash_table_index_mask];

    Identifier *id = nullptr;
    for (; index != 0xFFFFu; index = id->next) {
      id = &identifier_buffer[index];
      if (id->hash == hash && id->size == str_size && *id == str) [[likely]] {
        return index;
      }
    }
    return add_after(hash, str_view, id);
  }

  TokenAtom lookup_or_add(std::string_view str)
  {
    uint32_t hash = str_hash(str);
    uint16_t index = hash_table[hash & hash_table_index_mask];

    Identifier *id = nullptr;
    for (; index != 0xFFFFu; index = id->next) {
      id = &identifier_buffer[index];
      if (id->hash == hash && *id == str) [[likely]] {
        return index;
      }
    }
    return add_after(hash, str, id);
  }

  TokenAtom add_after(uint32_t hash, std::string_view str, Identifier *id = nullptr)
  {
    /* Cache miss. Add new. */
    uint16_t new_index = identifier_buffer.size();
    if (id) {
      /* Update previous element in the list. */
      id->next = new_index;
    }
    else {
      /* Update entry in table. */
      hash_table[hash & hash_table_index_mask] = new_index;
    }

    {
      /* Fast malloc replacement. */
      int str_as_id_size = ((str.size() + (sizeof(Identifier) - 1)) / sizeof(Identifier));
      identifier_buffer.reserve(new_index + 1 + str_as_id_size);
      Identifier &id = *identifier_buffer.end();
      identifier_buffer.increase_size_by_unchecked(1 + str_as_id_size);
      /* Construct new identifier. */
      id.next = 0xFFFFu;
      id.size = str.size();
      id.hash = hash;
      /* Zero the end of the memcpy for the fast comparison. */
      id.data[((str.size() - 1) / sizeof(Identifier))] = 0;
      std::memcpy(id.data, str.data(), str.size());
    }
    return new_index;
  }
};

/**
 * Based on this article.
 * https://lemire.me/blog/2022/12/30/quickly-checking-that-a-string-belongs-to-a-small-set/
 */
template<typename KeyT, typename ValueT, int Size, typename HashT> struct KeywordMap {
  std::array<TokenAtom, Size> value_map;
  std::array<KeyT, Size> match_table;

  constexpr TokenAtom lookup_default(KeyT str, ValueT default_value)
  {
    uint8_t hash = HashT::hash(str);
    bool match = match_table[hash] == str;
    /* Assuming the input is already a Word, lookup the 0th value on mismatch, which conveniently
     * is also a Word, resulting in a noop. */
    return match ? value_map[hash] : default_value;
  }
};

}  // namespace lexit
