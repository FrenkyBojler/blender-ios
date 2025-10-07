/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"
#include <cstring>

#include "BLI_fingerprint.hh"

namespace blender::tests {

/* Reverse the byte order of an unsigned 64-bit int. */
uint64_t swap_bytes(uint64_t n)
{
  uint64_t out = 0;
  for (int i = 0; i < 8; i++) {
    out <<= 8;
    out |= n & 0xff;
    n >>= 8;
  }
  return out;
}

TEST(BLI_fingerprint, hash)
{
  /* The standard test vectors for TentHash, with the digests truncated to 128
   * bits. */
  const static char inputs[][64] = {
      {},
      {0},
      "0123456789",
      "abcdefghijklmnopqrstuvwxyz",
      "This string is exactly 32 bytes.",
      "The quick brown fox jumps over the lazy dog.",
  };
  const static int inputs_len[] = {0, 1, 10, 26, 32, 44};
  const static uint64_t digests[][2] = {
      {swap_bytes(0x68c8213b7a76b8ed), swap_bytes(0x267dddb3d8717bb3)},
      {swap_bytes(0x3cf6833cca9c4d5e), swap_bytes(0x211318577bab74bf)},
      {swap_bytes(0xa7d324bde0bf6ce3), swap_bytes(0x427701628f0f8fc3)},
      {swap_bytes(0xf1be4be1a0f9eae6), swap_bytes(0x500fb2f6b64f3daa)},
      {swap_bytes(0xf7c5e4763d89bddc), swap_bytes(0xe33e97712b712d86)},
      {swap_bytes(0xde77f1c134228be1), swap_bytes(0xb5b25c941d5102f8)},
  };

  for (int i = 0; i < 6; i++) {
    const fingerprint::Digest digest = fingerprint::hash(inputs[i], inputs_len[i]);
    EXPECT_EQ(digest.low64, digests[i][0]);
    EXPECT_EQ(digest.high64, digests[i][1]);
  }
}

TEST(BLI_fingerprint, Hasher)
{
  /* The standard test vectors for TentHash, with the digests truncated to 128
   * bits. */
  const static char inputs[][64] = {
      {},
      {0},
      "0123456789",
      "abcdefghijklmnopqrstuvwxyz",
      "This string is exactly 32 bytes.",
      "The quick brown fox jumps over the lazy dog.",
  };
  const static int inputs_len[] = {0, 1, 10, 26, 32, 44};
  const static uint64_t digests[][2] = {
      {swap_bytes(0x68c8213b7a76b8ed), swap_bytes(0x267dddb3d8717bb3)},
      {swap_bytes(0x3cf6833cca9c4d5e), swap_bytes(0x211318577bab74bf)},
      {swap_bytes(0xa7d324bde0bf6ce3), swap_bytes(0x427701628f0f8fc3)},
      {swap_bytes(0xf1be4be1a0f9eae6), swap_bytes(0x500fb2f6b64f3daa)},
      {swap_bytes(0xf7c5e4763d89bddc), swap_bytes(0xe33e97712b712d86)},
      {swap_bytes(0xde77f1c134228be1), swap_bytes(0xb5b25c941d5102f8)},
  };

  for (int i = 0; i < 6; i++) {
    fingerprint::Hasher hasher;
    hasher.update(inputs[i], inputs_len[i]);
    const fingerprint::Digest digest = hasher.digest();

    EXPECT_EQ(digest.low64, digests[i][0]);
    EXPECT_EQ(digest.high64, digests[i][1]);
  }
}

TEST(BLI_fingerprint, Hasher_segmented)
{
  /* The standard test vectors for TentHash, with the digests truncated to 128
   * bits. */
  const static char inputs[][64] = {
      {},
      {0},
      "0123456789",
      "abcdefghijklmnopqrstuvwxyz",
      "This string is exactly 32 bytes.",
      "The quick brown fox jumps over the lazy dog.",
  };
  const static int inputs_len[] = {0, 1, 10, 26, 32, 44};
  const static uint64_t digests[][2] = {
      {swap_bytes(0x68c8213b7a76b8ed), swap_bytes(0x267dddb3d8717bb3)},
      {swap_bytes(0x3cf6833cca9c4d5e), swap_bytes(0x211318577bab74bf)},
      {swap_bytes(0xa7d324bde0bf6ce3), swap_bytes(0x427701628f0f8fc3)},
      {swap_bytes(0xf1be4be1a0f9eae6), swap_bytes(0x500fb2f6b64f3daa)},
      {swap_bytes(0xf7c5e4763d89bddc), swap_bytes(0xe33e97712b712d86)},
      {swap_bytes(0xde77f1c134228be1), swap_bytes(0xb5b25c941d5102f8)},
  };

  for (int i = 0; i < 6; i++) {
    fingerprint::Hasher hasher;

    /* Pass the data to the hasher 7 bytes at a time. */
    const char *data = inputs[i];
    int len = inputs_len[i];
    while (len > 0) {
      hasher.update(data, std::min(7, len));
      data += 7;
      len -= 7;
    }
    const fingerprint::Digest digest = hasher.digest();

    EXPECT_EQ(digest.low64, digests[i][0]);
    EXPECT_EQ(digest.high64, digests[i][1]);
  }
}

}  // namespace blender::tests
