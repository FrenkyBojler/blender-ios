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
  struct alignas(4) Identifier {
    uint16_t next;
    uint16_t size;
    char data[0];

    explicit operator std::string_view()
    {
      return {data, size};
    }
  };
  lexit::Vector<Identifier> identifier_buffer;
  /* Note: Must be power of two size. */
  std::array<uint16_t, 16384> hash_table;

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

  INLINE_METHOD TokenAtom lookup_or_add(uint16_t hash, std::string_view str)
  {
    hash &= (hash_table.size() - 1);
    uint16_t index = hash_table[hash];

    /* Move first iteration out of the loop as this is the most probable out case. */
    if (index != 0xFFFFu && std::string_view(identifier_buffer[index]) == str) [[likely]] {
      return index;
    }

    Identifier *id = nullptr;
    for (;;) {
      if (index == 0xFFFFu) [[unlikely]] {
        break;
      }
      id = &identifier_buffer[index];
      if (std::string_view(*id) == str) [[likely]] {
        return index;
      }
      index = id->next;
    }

    /* Cache miss. Add new. */
    uint16_t new_index = identifier_buffer.size();
    if (id) {
      /* Update previous element in the list. */
      id->next = new_index;
    }
    else {
      /* Update entry in table. */
      hash_table[hash] = new_index;
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
      std::memcpy(id.data, str.data(), str.size());
    }
    return new_index;
  }
};

}  // namespace lexit
