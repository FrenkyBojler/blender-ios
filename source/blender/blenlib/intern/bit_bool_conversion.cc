/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bit_bool_conversion.hh"
#include "BLI_simd.hh"

namespace blender::bits {

template<typename ByteToBit>
bool or_bytes_into_bits(const Span<char> bytes,
                        MutableBitSpan r_bits,
                        const int64_t allowed_overshoot,
                        const ByteToBit &byte_to_bit)
{
  BLI_assert(r_bits.size() >= bytes.size());
  if (bytes.is_empty()) {
    return false;
  }

  int64_t byte_i = 0;
  const char *bytes_ = bytes.data();

  bool any_true = false;

/* Conversion from bytes to bits can be way faster with intrinsics. That's because instead of
 * processing one element at a time, we can process 16 at once. */
#if BLI_HAVE_SSE2
  int64_t iteration_end = bytes.size();
  if (iteration_end % 16 > 0) {
    if (allowed_overshoot >= 16) {
      iteration_end = (iteration_end + 16) & ~15;
    }
  }
  /* Iterate over chunks of bytes. */
  for (; byte_i + 16 <= iteration_end; byte_i += 16) {
    /* Load 16 bytes at once. */
    const __m128i group = _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes_ + byte_i));
    const uint16_t is_true_mask = byte_to_bit.see2_chunk(group);
    any_true |= is_true_mask != 0;

    const int start_bit_in_int = (r_bits.bit_range().start() + byte_i) & BitIndexMask;
    BitInt *start_bit_int = int_containing_bit(r_bits.data(), r_bits.bit_range().start() + byte_i);
    *start_bit_int |= BitInt(is_true_mask) << start_bit_in_int;

    if (start_bit_in_int > BitsPerInt - 16) {
      /* It's possible that the bits need inserted in two consecutive integers. */
      start_bit_int[1] |= BitInt(is_true_mask) >> (64 - start_bit_in_int);
    }
  }
#endif

  /* Process remaining bytes. */
  for (; byte_i < bytes.size(); byte_i++) {
    if (byte_to_bit.single(bytes_[byte_i])) {
      r_bits[byte_i].set();
      any_true = true;
    }
  }
  return any_true;
}

struct BoolToBit {
  static bool single(const char c)
  {
    return bool(c);
  }

#if BLI_HAVE_SSE2
  static uint16_t see2_chunk(const __m128i chunk)
  {
    const __m128i zero_bytes = _mm_set1_epi8(0);
    /* Compare them all against zero. The result is a mask of the form [0x00, 0xff, 0xff, ...]. */
    const __m128i is_false_byte_mask = _mm_cmpeq_epi8(chunk, zero_bytes);
    /* Compress the byte-mask into a bit mask. This takes one bit from each byte. */
    const uint16_t is_false_mask = _mm_movemask_epi8(is_false_byte_mask);
    /* Now we have a bit mask where each bit corresponds to an input byte. */
    const uint16_t is_true_mask = ~is_false_mask;
    return is_true_mask;
  }
#endif
};

bool or_bools_into_bits(const Span<bool> bools,
                        MutableBitSpan r_bits,
                        const int64_t allowed_overshoot)
{
  return or_bytes_into_bits(bools.cast<char>(), r_bits, allowed_overshoot, BoolToBit());
}

template<int S> struct PredicateByteToBit {
  std::array<char, S> predicate_bytes;
#if BLI_HAVE_SSE2
  std::array<__m128i, S> predicate_chunks;
#endif

  PredicateByteToBit(const Span<char> &predicate_bytes)
  {
    BLI_assert(predicate_bytes.size() <= S);
    static_assert(S >= 1);
    for (int i = 0; i < S; i++) {
      const int src_i = i < predicate_bytes.size() ? i : 0;
      this->predicate_bytes[i] = predicate_bytes[src_i];
#if BLI_HAVE_SSE2
      this->predicate_chunks[i] = _mm_set1_epi8(predicate_bytes[src_i]);
#endif
    }
  }

  bool single(const char c) const
  {
    for (int i = 0; i < S; i++) {
      if (c == this->predicate_bytes[i]) {
        return true;
      }
    }
    return false;
  }

#if BLI_HAVE_SSE2
  uint16_t see2_chunk(const __m128i chunk) const
  {
    __m128i mask = _mm_cmpeq_epi8(chunk, this->predicate_chunks[0]);
    for (int i = 1; i < S; i++) {
      mask = _mm_or_si128(mask, _mm_cmpeq_epi8(chunk, this->predicate_chunks[i]));
    }
    return _mm_movemask_epi8(mask);
  }
#endif
};

bool bytes_to_bits(Span<char> bytes,
                   Span<char> predicate_bytes,
                   MutableBitSpan r_bits,
                   int64_t allowed_overshoot)
{
  if (predicate_bytes.is_empty()) {
    return false;
  }
  if (predicate_bytes.size() == 1) {
    return or_bytes_into_bits(
        bytes, r_bits, allowed_overshoot, PredicateByteToBit<1>(predicate_bytes));
  }
  if (predicate_bytes.size() <= 2) {
    return or_bytes_into_bits(
        bytes, r_bits, allowed_overshoot, PredicateByteToBit<2>(predicate_bytes));
  }
  if (predicate_bytes.size() <= 4) {
    return or_bytes_into_bits(
        bytes, r_bits, allowed_overshoot, PredicateByteToBit<4>(predicate_bytes));
  }
  if (predicate_bytes.size() <= 5) {
    return or_bytes_into_bits(
        bytes, r_bits, allowed_overshoot, PredicateByteToBit<5>(predicate_bytes));
  }
  if (predicate_bytes.size() <= 8) {
    return or_bytes_into_bits(
        bytes, r_bits, allowed_overshoot, PredicateByteToBit<8>(predicate_bytes));
  }
  /* Not yet implemented. */
  BLI_assert_unreachable();
  return false;
}

}  // namespace blender::bits
