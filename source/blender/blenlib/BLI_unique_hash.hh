/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include "BLI_vector.hh"

namespace blender {

/**
 * A hash that uniquely identifies data. The hash has to have enough bits to make collisions
 * practically impossible.
 */
struct UniqueHash {
  uint64_t v1;
  uint64_t v2;

  uint64_t hash() const
  {
    return v1;
  }
  friend bool operator==(const UniqueHash &a, const UniqueHash &b) = default;
};

/**
 * Used to collect data to build a #UniqueHash. It is more efficient to hash a span of bytes in
 * one step than to update a hash with more data piece by piece.
 */
struct UniqueHashBytes {
  Vector<std::byte, 256> data;
  template<typename T> void add(const T &value)
  {
    static_assert(std::is_trivial_v<T>);
    data.extend(reinterpret_cast<const std::byte *>(&value), sizeof(T));
  }
};

}  // namespace blender
