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

struct IdentifierMap {
  struct alignas(8) Identifier {
    uint16_t next;
    uint16_t size;
    uint32_t hash;
    uint64_t data[0];

    bool operator==(const uint64_t str[2]) const
    {
      if (size > 8) {
        return data[0] == str[0] && data[1] == str[1];
      }
      return data[0] == str[0];
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

  /* If unsafe is true, it means that caller ensures that the string is padded to 16 bytes.
   * In other term, that the string_view starts before the last 16 bytes of the base string. */
  template<bool Unsafe = false> INLINE_METHOD TokenAtom lookup_or_add(std::string_view str)
  {
    uint32_t hash = str_hash(str);
    uint16_t index = hash_table[hash & hash_table_index_mask];

    Identifier *id = nullptr;

    if constexpr (Unsafe) {
      /* Small identifier optimization. */
      if (str.size() <= 16) [[likely]] {
        /* Copy of the small string onto aligned bytes. This avoids the cost of calling memcmp. */
        uint64_t str_aligned_bytes[2];
        static const uint64_t mask_table[8] = {
            uint64_t(0xFFFFFFFFFFFFFFFF),
            uint64_t(0x00000000000000FF),
            uint64_t(0x000000000000FFFF),
            uint64_t(0x0000000000FFFFFF),
            uint64_t(0x00000000FFFFFFFF),
            uint64_t(0x000000FFFFFFFFFF),
            uint64_t(0x0000FFFFFFFFFFFF),
            uint64_t(0x00FFFFFFFFFFFFFF),
        };
        /* In order for this to not be slow, we need to copy a known quantity.
         * This is why the caller needs to ensure . */
        std::memcpy(&str_aligned_bytes, str.data(), sizeof(str_aligned_bytes));
        str_aligned_bytes[int(str.size() > 8)] &= mask_table[str.size() & 7];

        for (; index != 0xFFFFu; index = id->next) {
          id = &identifier_buffer[index];
          if (id->hash == hash && id->size == str.size() && *id == str_aligned_bytes) [[likely]] {
            return index;
          }
        }
        return add_after(hash, str, id);
      }
    }

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

}  // namespace lexit
