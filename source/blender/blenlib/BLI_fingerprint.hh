/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * A fast, robust, non-cryptographic, 128-bit fingerprinting hash.
 *
 * Currently implements TentHash (truncated to 128 bits):
 * https://github.com/cessen/tenthash
 *
 * The purpose of this hash is for generating unique, non-colliding hashes that
 * uniquely identify pieces of data.
 *
 * The full 128-bit digest should ALWAYS be used. E.g. only using 64 bits of the
 * hash output exposes you to potential hash collisions. If such collisions are
 * acceptable for your use case, then this hash is overkill anyway and you
 * should be using something else.
 *
 * Both `hash()` and `Hasher` below compute identical hashes. The difference is
 * that `Hasher` lets you pass the data to be hashed incrementally. If the data
 * to be hashed is contiguous in memory, prefer using `hash()` because it's a
 * bit faster due to having less bookkeeping overhead.
 */

#include <cstddef>
#include <cstdint>

namespace blender::fingerprint {

/**
 * A 128-bit fingerprinting hash digest.
 */
struct Digest {
  uint64_t low64;
  uint64_t high64;
};

/**
 * Compute a 128-bit hash for the given data.
 *
 * \param data: An array of input data.
 * \param data_len: The length of `data` in bytes.
 */
Digest hash(const void *data, const size_t data_len);

/**
 * A hasher that takes input data incrementally.
 */
class Hasher {
  uint64_t state[4] = {
      0x5d6daffc4411a967,
      0xe22d4dea68577f34,
      0xca50864d814cbc2e,
      0x894e29b9611eb173,
  };

  uint8_t buffer[256 / 8];
  size_t buffer_length = 0;

  size_t message_length = 0;

 public:
  /**
   * Append data to the stream of data being hashed.
   *
   * \param data: An array of input data.
   * \param data_len: The length of `data` in bytes.
   */
  void update(const void *data, const size_t len);

  /**
   * Compute and return the final fingerprinting hash digest.
   *
   * This invalidates the hasher's internal state, and it should not be used
   * again without first calling `reset()`.
   */
  Digest digest();

  /**
   * Reset the hasher's internal state to start from scratch computing a new
   * hash.
   */
  void reset();
};

}  // namespace blender::fingerprint
