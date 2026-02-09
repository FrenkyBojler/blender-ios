/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "lexit.hh"
#include "simd.hh"

#include <cassert>

#if defined(__clang__) || defined(__GNUC__)
#  define count_bits_i(i) __builtin_popcount(i)
#elif defined(_MSC_VER)
#  define count_bits_i(i) __popcnt(i)
#else
#  include <bitset>
#  define count_bits_i(i) (std::bitset<8>{i}.count())
#endif

namespace lexit {

/* Helper function to realloc aligned array keeping elem_count data. */
template<typename T>
void realloc_aligned_array(std::unique_ptr<T[]> &ptr, size_t elem_count, size_t new_size)
{
  assert(new_size > elem_count);
  std::unique_ptr<T[]> new_ptr(new (std::align_val_t{64}) T[new_size]);
  if (ptr) {
    std::memcpy(new_ptr.get(), ptr.get(), elem_count * sizeof(T));
  }
  ptr = std::move(new_ptr);
}

void TokenBuffer::clear()
{
  size_ = 0;
}

void TokenBuffer::reserve(const uint32_t count)
{
  if (allocated_size_ >= count + 1) {
    return;
  }
  allocated_size_ = count + 1;
  realloc_aligned_array(types_, size_ + 1, allocated_size_);
  realloc_aligned_array(offsets_, size_ + 1, allocated_size_);
  realloc_aligned_array(original_offsets_, size_ + 1, allocated_size_);
  realloc_aligned_array(atoms_, size_ + 1, allocated_size_);
}

#if defined(USE_NEON) || defined(USE_SSE4_2)

/* Shuffle table used for stream compaction.
 * For a given 8bit pattern (where each 1 bit represents an element to keep)
 * encode the index of the source register for each of the 8 destination registers.
 * Every 0 bit (representing a discarded element) will be sourced from the 0th element.
 * This is to be used with table. */
alignas(16) static const uint8_t shuffle_table_8[256][8] = {
    /* [0b00000000] = */ {0, 0, 0, 0, 0, 0, 0, 0},
    /* [0b00000001] = */ {0, 0, 0, 0, 0, 0, 0, 0},
    /* [0b00000010] = */ {1, 0, 0, 0, 0, 0, 0, 0},
    /* [0b00000011] = */ {0, 1, 0, 0, 0, 0, 0, 0},
    /* [0b00000100] = */ {2, 0, 0, 0, 0, 0, 0, 0},
    /* [0b00000101] = */ {0, 2, 0, 0, 0, 0, 0, 0},
    /* [0b00000110] = */ {1, 2, 0, 0, 0, 0, 0, 0},
    /* [0b00000111] = */ {0, 1, 2, 0, 0, 0, 0, 0},
    /* [0b00001000] = */ {3, 0, 0, 0, 0, 0, 0, 0},
    /* [0b00001001] = */ {0, 3, 0, 0, 0, 0, 0, 0},
    /* [0b00001010] = */ {1, 3, 0, 0, 0, 0, 0, 0},
    /* [0b00001011] = */ {0, 1, 3, 0, 0, 0, 0, 0},
    /* [0b00001100] = */ {2, 3, 0, 0, 0, 0, 0, 0},
    /* [0b00001101] = */ {0, 2, 3, 0, 0, 0, 0, 0},
    /* [0b00001110] = */ {1, 2, 3, 0, 0, 0, 0, 0},
    /* [0b00001111] = */ {0, 1, 2, 3, 0, 0, 0, 0},
    /* [0b00010000] = */ {4, 0, 0, 0, 0, 0, 0, 0},
    /* [0b00010001] = */ {0, 4, 0, 0, 0, 0, 0, 0},
    /* [0b00010010] = */ {1, 4, 0, 0, 0, 0, 0, 0},
    /* [0b00010011] = */ {0, 1, 4, 0, 0, 0, 0, 0},
    /* [0b00010100] = */ {2, 4, 0, 0, 0, 0, 0, 0},
    /* [0b00010101] = */ {0, 2, 4, 0, 0, 0, 0, 0},
    /* [0b00010110] = */ {1, 2, 4, 0, 0, 0, 0, 0},
    /* [0b00010111] = */ {0, 1, 2, 4, 0, 0, 0, 0},
    /* [0b00011000] = */ {3, 4, 0, 0, 0, 0, 0, 0},
    /* [0b00011001] = */ {0, 3, 4, 0, 0, 0, 0, 0},
    /* [0b00011010] = */ {1, 3, 4, 0, 0, 0, 0, 0},
    /* [0b00011011] = */ {0, 1, 3, 4, 0, 0, 0, 0},
    /* [0b00011100] = */ {2, 3, 4, 0, 0, 0, 0, 0},
    /* [0b00011101] = */ {0, 2, 3, 4, 0, 0, 0, 0},
    /* [0b00011110] = */ {1, 2, 3, 4, 0, 0, 0, 0},
    /* [0b00011111] = */ {0, 1, 2, 3, 4, 0, 0, 0},
    /* [0b00100000] = */ {5, 0, 0, 0, 0, 0, 0, 0},
    /* [0b00100001] = */ {0, 5, 0, 0, 0, 0, 0, 0},
    /* [0b00100010] = */ {1, 5, 0, 0, 0, 0, 0, 0},
    /* [0b00100011] = */ {0, 1, 5, 0, 0, 0, 0, 0},
    /* [0b00100100] = */ {2, 5, 0, 0, 0, 0, 0, 0},
    /* [0b00100101] = */ {0, 2, 5, 0, 0, 0, 0, 0},
    /* [0b00100110] = */ {1, 2, 5, 0, 0, 0, 0, 0},
    /* [0b00100111] = */ {0, 1, 2, 5, 0, 0, 0, 0},
    /* [0b00101000] = */ {3, 5, 0, 0, 0, 0, 0, 0},
    /* [0b00101001] = */ {0, 3, 5, 0, 0, 0, 0, 0},
    /* [0b00101010] = */ {1, 3, 5, 0, 0, 0, 0, 0},
    /* [0b00101011] = */ {0, 1, 3, 5, 0, 0, 0, 0},
    /* [0b00101100] = */ {2, 3, 5, 0, 0, 0, 0, 0},
    /* [0b00101101] = */ {0, 2, 3, 5, 0, 0, 0, 0},
    /* [0b00101110] = */ {1, 2, 3, 5, 0, 0, 0, 0},
    /* [0b00101111] = */ {0, 1, 2, 3, 5, 0, 0, 0},
    /* [0b00110000] = */ {4, 5, 0, 0, 0, 0, 0, 0},
    /* [0b00110001] = */ {0, 4, 5, 0, 0, 0, 0, 0},
    /* [0b00110010] = */ {1, 4, 5, 0, 0, 0, 0, 0},
    /* [0b00110011] = */ {0, 1, 4, 5, 0, 0, 0, 0},
    /* [0b00110100] = */ {2, 4, 5, 0, 0, 0, 0, 0},
    /* [0b00110101] = */ {0, 2, 4, 5, 0, 0, 0, 0},
    /* [0b00110110] = */ {1, 2, 4, 5, 0, 0, 0, 0},
    /* [0b00110111] = */ {0, 1, 2, 4, 5, 0, 0, 0},
    /* [0b00111000] = */ {3, 4, 5, 0, 0, 0, 0, 0},
    /* [0b00111001] = */ {0, 3, 4, 5, 0, 0, 0, 0},
    /* [0b00111010] = */ {1, 3, 4, 5, 0, 0, 0, 0},
    /* [0b00111011] = */ {0, 1, 3, 4, 5, 0, 0, 0},
    /* [0b00111100] = */ {2, 3, 4, 5, 0, 0, 0, 0},
    /* [0b00111101] = */ {0, 2, 3, 4, 5, 0, 0, 0},
    /* [0b00111110] = */ {1, 2, 3, 4, 5, 0, 0, 0},
    /* [0b00111111] = */ {0, 1, 2, 3, 4, 5, 0, 0},
    /* [0b01000000] = */ {6, 0, 0, 0, 0, 0, 0, 0},
    /* [0b01000001] = */ {0, 6, 0, 0, 0, 0, 0, 0},
    /* [0b01000010] = */ {1, 6, 0, 0, 0, 0, 0, 0},
    /* [0b01000011] = */ {0, 1, 6, 0, 0, 0, 0, 0},
    /* [0b01000100] = */ {2, 6, 0, 0, 0, 0, 0, 0},
    /* [0b01000101] = */ {0, 2, 6, 0, 0, 0, 0, 0},
    /* [0b01000110] = */ {1, 2, 6, 0, 0, 0, 0, 0},
    /* [0b01000111] = */ {0, 1, 2, 6, 0, 0, 0, 0},
    /* [0b01001000] = */ {3, 6, 0, 0, 0, 0, 0, 0},
    /* [0b01001001] = */ {0, 3, 6, 0, 0, 0, 0, 0},
    /* [0b01001010] = */ {1, 3, 6, 0, 0, 0, 0, 0},
    /* [0b01001011] = */ {0, 1, 3, 6, 0, 0, 0, 0},
    /* [0b01001100] = */ {2, 3, 6, 0, 0, 0, 0, 0},
    /* [0b01001101] = */ {0, 2, 3, 6, 0, 0, 0, 0},
    /* [0b01001110] = */ {1, 2, 3, 6, 0, 0, 0, 0},
    /* [0b01001111] = */ {0, 1, 2, 3, 6, 0, 0, 0},
    /* [0b01010000] = */ {4, 6, 0, 0, 0, 0, 0, 0},
    /* [0b01010001] = */ {0, 4, 6, 0, 0, 0, 0, 0},
    /* [0b01010010] = */ {1, 4, 6, 0, 0, 0, 0, 0},
    /* [0b01010011] = */ {0, 1, 4, 6, 0, 0, 0, 0},
    /* [0b01010100] = */ {2, 4, 6, 0, 0, 0, 0, 0},
    /* [0b01010101] = */ {0, 2, 4, 6, 0, 0, 0, 0},
    /* [0b01010110] = */ {1, 2, 4, 6, 0, 0, 0, 0},
    /* [0b01010111] = */ {0, 1, 2, 4, 6, 0, 0, 0},
    /* [0b01011000] = */ {3, 4, 6, 0, 0, 0, 0, 0},
    /* [0b01011001] = */ {0, 3, 4, 6, 0, 0, 0, 0},
    /* [0b01011010] = */ {1, 3, 4, 6, 0, 0, 0, 0},
    /* [0b01011011] = */ {0, 1, 3, 4, 6, 0, 0, 0},
    /* [0b01011100] = */ {2, 3, 4, 6, 0, 0, 0, 0},
    /* [0b01011101] = */ {0, 2, 3, 4, 6, 0, 0, 0},
    /* [0b01011110] = */ {1, 2, 3, 4, 6, 0, 0, 0},
    /* [0b01011111] = */ {0, 1, 2, 3, 4, 6, 0, 0},
    /* [0b01100000] = */ {5, 6, 0, 0, 0, 0, 0, 0},
    /* [0b01100001] = */ {0, 5, 6, 0, 0, 0, 0, 0},
    /* [0b01100010] = */ {1, 5, 6, 0, 0, 0, 0, 0},
    /* [0b01100011] = */ {0, 1, 5, 6, 0, 0, 0, 0},
    /* [0b01100100] = */ {2, 5, 6, 0, 0, 0, 0, 0},
    /* [0b01100101] = */ {0, 2, 5, 6, 0, 0, 0, 0},
    /* [0b01100110] = */ {1, 2, 5, 6, 0, 0, 0, 0},
    /* [0b01100111] = */ {0, 1, 2, 5, 6, 0, 0, 0},
    /* [0b01101000] = */ {3, 5, 6, 0, 0, 0, 0, 0},
    /* [0b01101001] = */ {0, 3, 5, 6, 0, 0, 0, 0},
    /* [0b01101010] = */ {1, 3, 5, 6, 0, 0, 0, 0},
    /* [0b01101011] = */ {0, 1, 3, 5, 6, 0, 0, 0},
    /* [0b01101100] = */ {2, 3, 5, 6, 0, 0, 0, 0},
    /* [0b01101101] = */ {0, 2, 3, 5, 6, 0, 0, 0},
    /* [0b01101110] = */ {1, 2, 3, 5, 6, 0, 0, 0},
    /* [0b01101111] = */ {0, 1, 2, 3, 5, 6, 0, 0},
    /* [0b01110000] = */ {4, 5, 6, 0, 0, 0, 0, 0},
    /* [0b01110001] = */ {0, 4, 5, 6, 0, 0, 0, 0},
    /* [0b01110010] = */ {1, 4, 5, 6, 0, 0, 0, 0},
    /* [0b01110011] = */ {0, 1, 4, 5, 6, 0, 0, 0},
    /* [0b01110100] = */ {2, 4, 5, 6, 0, 0, 0, 0},
    /* [0b01110101] = */ {0, 2, 4, 5, 6, 0, 0, 0},
    /* [0b01110110] = */ {1, 2, 4, 5, 6, 0, 0, 0},
    /* [0b01110111] = */ {0, 1, 2, 4, 5, 6, 0, 0},
    /* [0b01111000] = */ {3, 4, 5, 6, 0, 0, 0, 0},
    /* [0b01111001] = */ {0, 3, 4, 5, 6, 0, 0, 0},
    /* [0b01111010] = */ {1, 3, 4, 5, 6, 0, 0, 0},
    /* [0b01111011] = */ {0, 1, 3, 4, 5, 6, 0, 0},
    /* [0b01111100] = */ {2, 3, 4, 5, 6, 0, 0, 0},
    /* [0b01111101] = */ {0, 2, 3, 4, 5, 6, 0, 0},
    /* [0b01111110] = */ {1, 2, 3, 4, 5, 6, 0, 0},
    /* [0b01111111] = */ {0, 1, 2, 3, 4, 5, 6, 0},
    /* [0b10000000] = */ {7, 0, 0, 0, 0, 0, 0, 0},
    /* [0b10000001] = */ {0, 7, 0, 0, 0, 0, 0, 0},
    /* [0b10000010] = */ {1, 7, 0, 0, 0, 0, 0, 0},
    /* [0b10000011] = */ {0, 1, 7, 0, 0, 0, 0, 0},
    /* [0b10000100] = */ {2, 7, 0, 0, 0, 0, 0, 0},
    /* [0b10000101] = */ {0, 2, 7, 0, 0, 0, 0, 0},
    /* [0b10000110] = */ {1, 2, 7, 0, 0, 0, 0, 0},
    /* [0b10000111] = */ {0, 1, 2, 7, 0, 0, 0, 0},
    /* [0b10001000] = */ {3, 7, 0, 0, 0, 0, 0, 0},
    /* [0b10001001] = */ {0, 3, 7, 0, 0, 0, 0, 0},
    /* [0b10001010] = */ {1, 3, 7, 0, 0, 0, 0, 0},
    /* [0b10001011] = */ {0, 1, 3, 7, 0, 0, 0, 0},
    /* [0b10001100] = */ {2, 3, 7, 0, 0, 0, 0, 0},
    /* [0b10001101] = */ {0, 2, 3, 7, 0, 0, 0, 0},
    /* [0b10001110] = */ {1, 2, 3, 7, 0, 0, 0, 0},
    /* [0b10001111] = */ {0, 1, 2, 3, 7, 0, 0, 0},
    /* [0b10010000] = */ {4, 7, 0, 0, 0, 0, 0, 0},
    /* [0b10010001] = */ {0, 4, 7, 0, 0, 0, 0, 0},
    /* [0b10010010] = */ {1, 4, 7, 0, 0, 0, 0, 0},
    /* [0b10010011] = */ {0, 1, 4, 7, 0, 0, 0, 0},
    /* [0b10010100] = */ {2, 4, 7, 0, 0, 0, 0, 0},
    /* [0b10010101] = */ {0, 2, 4, 7, 0, 0, 0, 0},
    /* [0b10010110] = */ {1, 2, 4, 7, 0, 0, 0, 0},
    /* [0b10010111] = */ {0, 1, 2, 4, 7, 0, 0, 0},
    /* [0b10011000] = */ {3, 4, 7, 0, 0, 0, 0, 0},
    /* [0b10011001] = */ {0, 3, 4, 7, 0, 0, 0, 0},
    /* [0b10011010] = */ {1, 3, 4, 7, 0, 0, 0, 0},
    /* [0b10011011] = */ {0, 1, 3, 4, 7, 0, 0, 0},
    /* [0b10011100] = */ {2, 3, 4, 7, 0, 0, 0, 0},
    /* [0b10011101] = */ {0, 2, 3, 4, 7, 0, 0, 0},
    /* [0b10011110] = */ {1, 2, 3, 4, 7, 0, 0, 0},
    /* [0b10011111] = */ {0, 1, 2, 3, 4, 7, 0, 0},
    /* [0b10100000] = */ {5, 7, 0, 0, 0, 0, 0, 0},
    /* [0b10100001] = */ {0, 5, 7, 0, 0, 0, 0, 0},
    /* [0b10100010] = */ {1, 5, 7, 0, 0, 0, 0, 0},
    /* [0b10100011] = */ {0, 1, 5, 7, 0, 0, 0, 0},
    /* [0b10100100] = */ {2, 5, 7, 0, 0, 0, 0, 0},
    /* [0b10100101] = */ {0, 2, 5, 7, 0, 0, 0, 0},
    /* [0b10100110] = */ {1, 2, 5, 7, 0, 0, 0, 0},
    /* [0b10100111] = */ {0, 1, 2, 5, 7, 0, 0, 0},
    /* [0b10101000] = */ {3, 5, 7, 0, 0, 0, 0, 0},
    /* [0b10101001] = */ {0, 3, 5, 7, 0, 0, 0, 0},
    /* [0b10101010] = */ {1, 3, 5, 7, 0, 0, 0, 0},
    /* [0b10101011] = */ {0, 1, 3, 5, 7, 0, 0, 0},
    /* [0b10101100] = */ {2, 3, 5, 7, 0, 0, 0, 0},
    /* [0b10101101] = */ {0, 2, 3, 5, 7, 0, 0, 0},
    /* [0b10101110] = */ {1, 2, 3, 5, 7, 0, 0, 0},
    /* [0b10101111] = */ {0, 1, 2, 3, 5, 7, 0, 0},
    /* [0b10110000] = */ {4, 5, 7, 0, 0, 0, 0, 0},
    /* [0b10110001] = */ {0, 4, 5, 7, 0, 0, 0, 0},
    /* [0b10110010] = */ {1, 4, 5, 7, 0, 0, 0, 0},
    /* [0b10110011] = */ {0, 1, 4, 5, 7, 0, 0, 0},
    /* [0b10110100] = */ {2, 4, 5, 7, 0, 0, 0, 0},
    /* [0b10110101] = */ {0, 2, 4, 5, 7, 0, 0, 0},
    /* [0b10110110] = */ {1, 2, 4, 5, 7, 0, 0, 0},
    /* [0b10110111] = */ {0, 1, 2, 4, 5, 7, 0, 0},
    /* [0b10111000] = */ {3, 4, 5, 7, 0, 0, 0, 0},
    /* [0b10111001] = */ {0, 3, 4, 5, 7, 0, 0, 0},
    /* [0b10111010] = */ {1, 3, 4, 5, 7, 0, 0, 0},
    /* [0b10111011] = */ {0, 1, 3, 4, 5, 7, 0, 0},
    /* [0b10111100] = */ {2, 3, 4, 5, 7, 0, 0, 0},
    /* [0b10111101] = */ {0, 2, 3, 4, 5, 7, 0, 0},
    /* [0b10111110] = */ {1, 2, 3, 4, 5, 7, 0, 0},
    /* [0b10111111] = */ {0, 1, 2, 3, 4, 5, 7, 0},
    /* [0b11000000] = */ {6, 7, 0, 0, 0, 0, 0, 0},
    /* [0b11000001] = */ {0, 6, 7, 0, 0, 0, 0, 0},
    /* [0b11000010] = */ {1, 6, 7, 0, 0, 0, 0, 0},
    /* [0b11000011] = */ {0, 1, 6, 7, 0, 0, 0, 0},
    /* [0b11000100] = */ {2, 6, 7, 0, 0, 0, 0, 0},
    /* [0b11000101] = */ {0, 2, 6, 7, 0, 0, 0, 0},
    /* [0b11000110] = */ {1, 2, 6, 7, 0, 0, 0, 0},
    /* [0b11000111] = */ {0, 1, 2, 6, 7, 0, 0, 0},
    /* [0b11001000] = */ {3, 6, 7, 0, 0, 0, 0, 0},
    /* [0b11001001] = */ {0, 3, 6, 7, 0, 0, 0, 0},
    /* [0b11001010] = */ {1, 3, 6, 7, 0, 0, 0, 0},
    /* [0b11001011] = */ {0, 1, 3, 6, 7, 0, 0, 0},
    /* [0b11001100] = */ {2, 3, 6, 7, 0, 0, 0, 0},
    /* [0b11001101] = */ {0, 2, 3, 6, 7, 0, 0, 0},
    /* [0b11001110] = */ {1, 2, 3, 6, 7, 0, 0, 0},
    /* [0b11001111] = */ {0, 1, 2, 3, 6, 7, 0, 0},
    /* [0b11010000] = */ {4, 6, 7, 0, 0, 0, 0, 0},
    /* [0b11010001] = */ {0, 4, 6, 7, 0, 0, 0, 0},
    /* [0b11010010] = */ {1, 4, 6, 7, 0, 0, 0, 0},
    /* [0b11010011] = */ {0, 1, 4, 6, 7, 0, 0, 0},
    /* [0b11010100] = */ {2, 4, 6, 7, 0, 0, 0, 0},
    /* [0b11010101] = */ {0, 2, 4, 6, 7, 0, 0, 0},
    /* [0b11010110] = */ {1, 2, 4, 6, 7, 0, 0, 0},
    /* [0b11010111] = */ {0, 1, 2, 4, 6, 7, 0, 0},
    /* [0b11011000] = */ {3, 4, 6, 7, 0, 0, 0, 0},
    /* [0b11011001] = */ {0, 3, 4, 6, 7, 0, 0, 0},
    /* [0b11011010] = */ {1, 3, 4, 6, 7, 0, 0, 0},
    /* [0b11011011] = */ {0, 1, 3, 4, 6, 7, 0, 0},
    /* [0b11011100] = */ {2, 3, 4, 6, 7, 0, 0, 0},
    /* [0b11011101] = */ {0, 2, 3, 4, 6, 7, 0, 0},
    /* [0b11011110] = */ {1, 2, 3, 4, 6, 7, 0, 0},
    /* [0b11011111] = */ {0, 1, 2, 3, 4, 6, 7, 0},
    /* [0b11100000] = */ {5, 6, 7, 0, 0, 0, 0, 0},
    /* [0b11100001] = */ {0, 5, 6, 7, 0, 0, 0, 0},
    /* [0b11100010] = */ {1, 5, 6, 7, 0, 0, 0, 0},
    /* [0b11100011] = */ {0, 1, 5, 6, 7, 0, 0, 0},
    /* [0b11100100] = */ {2, 5, 6, 7, 0, 0, 0, 0},
    /* [0b11100101] = */ {0, 2, 5, 6, 7, 0, 0, 0},
    /* [0b11100110] = */ {1, 2, 5, 6, 7, 0, 0, 0},
    /* [0b11100111] = */ {0, 1, 2, 5, 6, 7, 0, 0},
    /* [0b11101000] = */ {3, 5, 6, 7, 0, 0, 0, 0},
    /* [0b11101001] = */ {0, 3, 5, 6, 7, 0, 0, 0},
    /* [0b11101010] = */ {1, 3, 5, 6, 7, 0, 0, 0},
    /* [0b11101011] = */ {0, 1, 3, 5, 6, 7, 0, 0},
    /* [0b11101100] = */ {2, 3, 5, 6, 7, 0, 0, 0},
    /* [0b11101101] = */ {0, 2, 3, 5, 6, 7, 0, 0},
    /* [0b11101110] = */ {1, 2, 3, 5, 6, 7, 0, 0},
    /* [0b11101111] = */ {0, 1, 2, 3, 5, 6, 7, 0},
    /* [0b11110000] = */ {4, 5, 6, 7, 0, 0, 0, 0},
    /* [0b11110001] = */ {0, 4, 5, 6, 7, 0, 0, 0},
    /* [0b11110010] = */ {1, 4, 5, 6, 7, 0, 0, 0},
    /* [0b11110011] = */ {0, 1, 4, 5, 6, 7, 0, 0},
    /* [0b11110100] = */ {2, 4, 5, 6, 7, 0, 0, 0},
    /* [0b11110101] = */ {0, 2, 4, 5, 6, 7, 0, 0},
    /* [0b11110110] = */ {1, 2, 4, 5, 6, 7, 0, 0},
    /* [0b11110111] = */ {0, 1, 2, 4, 5, 6, 7, 0},
    /* [0b11111000] = */ {3, 4, 5, 6, 7, 0, 0, 0},
    /* [0b11111001] = */ {0, 3, 4, 5, 6, 7, 0, 0},
    /* [0b11111010] = */ {1, 3, 4, 5, 6, 7, 0, 0},
    /* [0b11111011] = */ {0, 1, 3, 4, 5, 6, 7, 0},
    /* [0b11111100] = */ {2, 3, 4, 5, 6, 7, 0, 0},
    /* [0b11111101] = */ {0, 2, 3, 4, 5, 6, 7, 0},
    /* [0b11111110] = */ {1, 2, 3, 4, 5, 6, 7, 0},
    /* [0b11111111] = */ {0, 1, 2, 3, 4, 5, 6, 7},
};

/* Popcount for a uint8_t. */
alignas(16) static const uint8_t mask_popcount[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3,
    4, 4, 5, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4,
    4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4,
    5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5,
    4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2,
    3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5,
    5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4,
    5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 3, 4, 4, 5, 4, 5, 5, 6,
    4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};

#endif

void TokenBuffer::tokenize(const CharClass char_class_table[128])
{
  uint32_t offset = 0, cursor = 0;

  const uint8_t *str = (const uint8_t *)str_.data();

  CharClass prev_value = CharClass::None;

#if defined(USE_SSE4_2) || defined(USE_NEON)
  using namespace lexit::simd;

  const u8x128_table char_to_class = u8x128_table::load((const uint8_t *)char_class_table);

  const u8x16 to_type_threshold{uint8_t(CharClass::ClassToTypeThreshold)};
  const u8x16 can_merge{uint8_t(CharClass::CanMerge)};

  for (; offset + 16 <= str_.size(); offset += 16) {
    /* Load 16 chars. */
    const u8x16 c = u8x16::load(str + offset);
    /* Lookup their class. */
    const u8x16 curr = char_to_class[c];
    /* (curr > ClassToTypeThreshold) ? curr : c */
    const u8x16 type = select(c, curr, curr > to_type_threshold);
    /* Shift and add the last iteration end token at the start of the vector. */
    const u8x16 prev = shift_lanes_right<1>(curr, uint8_t(prev_value));
    /* Equivalent to: `!bool(curr & prev & CanMerge)`. */
    const u8x16 emit = is_zero(curr & prev & can_merge);
    /* Make it 1 bit valid element flag. */
    const uint16_t emit_mask = movemask(emit);
    /* Store for next iteration. */
    prev_value = CharClass(curr.last());

    /* Stream compaction of data based on the emit mask (0xFF == emit, 0x00 == skip).
     * Stores `data` compacted inside `data_out` starting from `data_out + cursor` and advance
     * `cursor` by the number of element compacted. */
    {
      /* Lookup the shuffle vector in 2 halves. */
      const uint8_t emit_mask_lo = emit_mask & 0xFFu;
      const uint8_t emit_mask_hi = emit_mask >> 8;
      const uint8_t mask_popcount_lo = mask_popcount[emit_mask_lo];
      const uint8_t mask_popcount_hi = mask_popcount[emit_mask_hi];

      /* TODO MSVC: compat. */
      const unsigned __int128 v0 = *(uint64_t *)shuffle_table_8[emit_mask_lo];
      const unsigned __int128 v1 = *(uint64_t *)shuffle_table_8[emit_mask_hi] |
                                   uint64_t(0x0808080808080808);
      /* Combine shuffle vectors to remove holes. */
      alignas(16) const unsigned __int128 combined = v0 | (v1 << (mask_popcount_lo * 8));

      u8x16 shuffle = u8x16::load((const uint8_t *)&combined);

      /* Move data to destination elements (compaction). */
      u8x16 data_packed = u8x16_table(type).unsafe_shuffle(shuffle);
      /* Write 16 types in the stream. */
      data_packed.store_unaligned((uint8_t *)types_.get() + cursor);

      /* The offsets are contained inside the 8 bit shuffle vector.
       * We need to promote it to 32 bit before adding the base offset. */
      u32x16 shuffle32x16 = u32x16(shuffle) + offset;
      /* Write 16 offsets. */
      shuffle32x16.store_unaligned(offsets_.get() + cursor);

      cursor += mask_popcount_lo + mask_popcount_hi;
    }
  }
  /* Finish tail using scalar loop. */
#endif

  for (; offset < str_.size(); offset += 1) {
    const char c = str_[offset];
    const CharClass curr = char_class_table[c];
    /* It is faster to overwrite the previous value with the same value
     * as having a condition. */
    types_[cursor] = (curr > CharClass::ClassToTypeThreshold) ? TokenType(curr) : TokenType(c);
    offsets_[cursor] = offset;
    /* Split if no class in common. */
    cursor += (uint8_t(curr) & uint8_t(prev_value) & uint8_t(CharClass::CanMerge)) == 0;
    prev_value = curr;
  }

  /* Set end of last token. */
  offsets_[cursor] = str_.size();
  /* Set end of file token. */
  types_[cursor] = EndOfFile;

  size_ = cursor;
  whitespaces_collapsed_ = false;
}

TokenType get_stored_type(char char_value, CharClass char_class)
{
  return (char_class > CharClass::ClassToTypeThreshold) ? TokenType(char_class) :
                                                          TokenType(char_value);
}

void TokenBuffer::tokenize_without_whitespace(const CharClass char_class_table[128])
{
  uint32_t offset = 0, cursor = 0, original_cursor = 0;

  /* Scalar only implementation. */
  CharClass last_type = CharClass::None;

  {
    CharClass prev = last_type;
    /* Always emit first token. */
    for (; offset < 1; offset += 1) {
      const char c = str_[offset];
      const CharClass curr = char_class_table[c];
      /* It is faster to overwrite the previous value with the same value
       * than having a condition. */
      types_[cursor] = get_stored_type(c, curr);
      offsets_[cursor] = offset;
      original_offsets_[original_cursor] = offset;
      /* Split if no class in common. */
      cursor += 1;
      original_cursor += 1;
      prev = curr;
    }
    bool prev_not_whitespace = false;
    for (; offset < str_.size(); offset += 1) {
      const char c = str_[offset];
      const CharClass curr = char_class_table[c];
      /* It is faster to overwrite the previous value with the same value
       * than having a condition. */
      types_[cursor] = get_stored_type(c, curr);
      offsets_[cursor] = offset;
      original_offsets_[original_cursor] = offset;
      const bool curr_not_whitespace = (curr != CharClass::WhiteSpace);
      const bool emit = (uint8_t(curr) & uint8_t(prev) & uint8_t(CharClass::CanMerge)) == 0;
      const bool emit_non_whitespace = emit && curr_not_whitespace;
      const bool follow_non_whitespace = curr_not_whitespace && prev_not_whitespace;
      const bool emit_whitespace = emit && !curr_not_whitespace;
      const bool emit_orig_offset = emit_whitespace ||
                                    (follow_non_whitespace && emit_non_whitespace);
      prev_not_whitespace = curr_not_whitespace;
      /* Split if no class in common. */
      cursor += emit_non_whitespace;
      original_cursor += emit_orig_offset;
      prev = curr;
    }
  }

  /* Set end of last token. */
  offsets_[cursor] = str_.size();
  original_offsets_[cursor] = str_.size();
  /* Set end of file token. */
  types_[cursor] = EndOfFile;

  size_ = cursor;
  whitespaces_collapsed_ = true;
}

static void lex_string(const TokenType *types, uint32_t &cursor)
{
  const TokenType *ptr = types + cursor;
  while (true) {
    cursor++;
    ptr++;
    if (*ptr == '\\') {
      /* Escaped character. Skip next. */
      cursor++;
      ptr++;
      continue;
    }
    if (*ptr == String || *ptr == EndOfFile) {
      return;
    }
  }
}

static void lex_number(const std::string_view str,
                       const TokenType *types,
                       const uint32_t *offsets,
                       uint32_t &cursor)
{
  const TokenType *type = types + cursor;
  const uint32_t *offset = offsets + cursor;
  while (true) {
    cursor++;
    type++;
    offset++;
    /* Check if the previous char was an exponent "e" char. */
    if ((*type == '+' || *type == '-') && str[*offset - 1] != 'e') {
      break;
    }
    if (!(*type == Word || *type == Number || *type == '.' || *type == '+' || *type == '-')) {
      break;
    }
  }
  /* We need to evaluate the token we broke on. */
  cursor--;
}

void TokenBuffer::merge_complex_literals()
{
  const TokenType *in_types = types_.get();
  TokenType *out_type = types_.get();
  const uint32_t *in_offsets = offsets_.get();
  uint32_t *out_offset = offsets_.get();

  for (uint32_t i = 0; i < size_; i++, out_type++, out_offset++) {
    const TokenType type = in_types[i];
    const uint32_t offset = in_offsets[i];
    *out_type = type;
    *out_offset = offset;

    switch (type) {
      case String:
        lex_string(in_types, i);
        break;
      case Number:
        lex_number(str_, in_types, in_offsets, i);
        break;
      default:
        break;
    }
  }

  assert(in_types < out_type);
  assert(out_type - in_types < 0xFFFFFFFFu);
  size_ = out_type - in_types;
  types_[size_] = EndOfFile;
  offsets_[size_] = str_.size();
}

template<TokenType removed_type, TokenType removed_type2 = removed_type>
static uint32_t merge_token(const TokenType *in_types,
                            const uint32_t *in_offsets,
                            TokenType *out_types,
                            uint32_t *out_offsets,
                            uint32_t *out_original_offsets,
                            const uint32_t token_count,
                            const uint32_t str_size)
{
  uint32_t j = 0;
  out_original_offsets[j] = 0;

  if (token_count > 0) {
    /* Iter 0 never merges. */
    const TokenType type = in_types[0];
    const uint32_t offset = in_offsets[0];
    const uint32_t next_offset = in_offsets[0 + 1];

    out_types[j] = type;
    out_offsets[j] = offset;
    out_original_offsets[j + 1] = next_offset;
    j++;
  }

  for (uint32_t i = 1; i < token_count; i++) {
    const TokenType type = in_types[i];
    const uint32_t offset = in_offsets[i];
    const uint32_t next_offset = in_offsets[i + 1];

    out_types[j] = type;
    out_offsets[j] = offset;
    out_original_offsets[j + 1] = next_offset;
    /* If false, make the next token overwrite this one.
     * Effectively merging the token with the one before. */
    j += int(type != removed_type && type != removed_type2);
  }

  out_types[j] = EndOfFile;
  out_offsets[j] = str_size;
  out_original_offsets[j] = str_size;

  return j;
}

void TokenBuffer::merge_whitespaces()
{
  size_ = merge_token<Space, NewLine>(types_.get(),
                                      offsets_.get(),
                                      types_.get(),
                                      offsets_.get(),
                                      original_offsets_.get(),
                                      size_,
                                      str_.size());
  whitespaces_collapsed_ = true;
}

void TokenBuffer::merge_spaces()
{
  size_ = merge_token<Space>(types_.get(),
                             offsets_.get(),
                             types_.get(),
                             offsets_.get(),
                             original_offsets_.get(),
                             size_,
                             str_.size());
  whitespaces_collapsed_ = true;
}

}  // namespace lexit
