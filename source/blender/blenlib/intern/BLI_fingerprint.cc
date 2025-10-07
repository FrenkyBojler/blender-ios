/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include "BLI_fingerprint.hh"

#define BLOCK_SIZE (256 / 8)
#define ROTL64(x, n) ((x << n) | (x >> (64 - n)))

namespace blender::fingerprint {

static void mix_state(uint64_t state[4])
{
  /* Rotation constants. */
  const static int rots[7][2] = {
      {16, 28},
      {14, 57},
      {11, 22},
      {35, 34},
      {57, 16},
      {59, 40},
      {44, 13},
  };

  for (int i = 0; i < 7; i++) {
    state[0] += state[2];
    state[1] += state[3];
    state[2] = ROTL64(state[2], rots[i][0]) ^ state[0];
    state[3] = ROTL64(state[3], rots[i][1]) ^ state[1];

    std::swap(state[0], state[1]);
  }
}

Digest hash(const void *data, const size_t data_len)
{
  uint64_t len = data_len;
  const uint8_t *data_u8 = (const uint8_t *)data;

  uint64_t state[4] = {
      0x5d6daffc4411a967,
      0xe22d4dea68577f34,
      0xca50864d814cbc2e,
      0x894e29b9611eb173,
  };

  /* Process the input data in 256-bit blocks. */
  while (len >= BLOCK_SIZE) {
    state[0] ^= *reinterpret_cast<const uint64_t *>(data_u8 + 0);
    state[1] ^= *reinterpret_cast<const uint64_t *>(data_u8 + 8);
    state[2] ^= *reinterpret_cast<const uint64_t *>(data_u8 + 16);
    state[3] ^= *reinterpret_cast<const uint64_t *>(data_u8 + 24);

    data_u8 += BLOCK_SIZE;
    len -= BLOCK_SIZE;

    mix_state(state);
  }

  /* Handle any remaining data less than 256 bits. */
  if (len > 0) {
    /* Copy the data into a zeroed-out buffer. When the data is less than
     * 256 bits this pads it out to 256 bits with zeros. */
    uint8_t buffer[BLOCK_SIZE] = {0};
    memcpy(buffer, data_u8, len);

    state[0] ^= *reinterpret_cast<uint64_t *>(buffer + 0);
    state[1] ^= *reinterpret_cast<uint64_t *>(buffer + 8);
    state[2] ^= *reinterpret_cast<uint64_t *>(buffer + 16);
    state[3] ^= *reinterpret_cast<uint64_t *>(buffer + 24);

    mix_state(state);
  }

  /* Finalize. */
  state[0] ^= data_len * 8;
  mix_state(state);
  mix_state(state);

  return Digest{state[0], state[1]};
}

void Hasher::reset()
{
  this->state[0] = 0x5d6daffc4411a967;
  this->state[1] = 0xe22d4dea68577f34;
  this->state[2] = 0xca50864d814cbc2e;
  this->state[3] = 0x894e29b9611eb173;
  this->buffer_length = 0;
  this->message_length = 0;
}

void Hasher::update(const void *data, const size_t data_len)
{
  uint64_t len = data_len;
  const uint8_t *data_u8 = (const uint8_t *)data;

  this->message_length += len;

  while (len > 0) {
    if (this->buffer_length == 0 && len >= BLOCK_SIZE) {
      /* Process data directly, skipping the buffer. */
      this->state[0] ^= *reinterpret_cast<const uint64_t *>(data_u8 + 0);
      this->state[1] ^= *reinterpret_cast<const uint64_t *>(data_u8 + 8);
      this->state[2] ^= *reinterpret_cast<const uint64_t *>(data_u8 + 16);
      this->state[3] ^= *reinterpret_cast<const uint64_t *>(data_u8 + 24);

      mix_state(this->state);
      data_u8 += BLOCK_SIZE;
      len -= BLOCK_SIZE;
    }
    else if (this->buffer_length == BLOCK_SIZE) {
      /* Process the filled buffer. */
      this->state[0] ^= *reinterpret_cast<const uint64_t *>(this->buffer + 0);
      this->state[1] ^= *reinterpret_cast<const uint64_t *>(this->buffer + 8);
      this->state[2] ^= *reinterpret_cast<const uint64_t *>(this->buffer + 16);
      this->state[3] ^= *reinterpret_cast<const uint64_t *>(this->buffer + 24);

      mix_state(this->state);
      this->buffer_length = 0;
    }
    else {
      /* Fill the buffer. */
      const size_t n = std::min(BLOCK_SIZE - this->buffer_length, len);
      memcpy(this->buffer + this->buffer_length, data_u8, n);

      data_u8 += n;
      len -= n;
      this->buffer_length += n;
    }
  }
}

Digest Hasher::digest()
{
  /* Hash the remaining bytes if there are any. */
  if (this->buffer_length > 0) {
    /* Pad with zeros as needed. */
    memset(this->buffer + this->buffer_length, 0, BLOCK_SIZE - this->buffer_length);

    this->state[0] ^= *reinterpret_cast<const uint64_t *>(this->buffer + 0);
    this->state[1] ^= *reinterpret_cast<const uint64_t *>(this->buffer + 8);
    this->state[2] ^= *reinterpret_cast<const uint64_t *>(this->buffer + 16);
    this->state[3] ^= *reinterpret_cast<const uint64_t *>(this->buffer + 24);

    mix_state(this->state);
  }

  /* Incorporate the message length (in bits) and do the
   * final mixing. */
  this->state[0] ^= this->message_length * 8;
  mix_state(this->state);
  mix_state(this->state);

  return Digest{this->state[0], this->state[1]};
}

}  // namespace blender::fingerprint
