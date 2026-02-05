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
static const uint8_t shuffle_table_8[256][8] = {
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

static inline uint8x16_t simd_transform16_ascii(const uint8x16x4_t table[2],
                                                const uint8x16_t input)
{
  uint8x16_t result;

#  if defined(USE_NEON)
  /* Perform a 128 bytes table lookup of for 16 element.
   * https://lemire.me/blog/2019/07/23/arbitrary-byte-to-byte-maps-using-arm-neon/
   * Table lookup on NEON will return 0 on overflow.
   * Leverage this using XOR to swap which range we are looking up and combine result using OR. */
  const uint8x16_t t1 = table_lookup_8x16x4(table[0], input);
  const uint8x16_t t2 = table_lookup_8x16x4(table[1], bit_xor(input, make8x16(0x40)));
  result = bit_or(t1, t2);

#  elif defined(USE_SSE4_2)
  result = zero8x16();
  uint8x16_t high_nibble_mask = make8x16(0xF0);
  /* This replaces both vqtbl4q_u8 calls and the XOR/OR logic.
   * It covers the full ASCII range (0-127). */
  for (int i = 0; i < 8; ++i) {
    /* Identify which bytes in 'input' fall in the current 16-byte range
     * Range i=0 is 0-15 (0x00), i=1 is 16-31 (0x10), ..., i=7 is 112-127 (0x70) */
    uint8x16_t range_match = equal(bit_and(input, high_nibble_mask), make8x16(i << 4));
    /* Perform the shuffle. _mm_shuffle_epi8 only uses the low 4 bits of the index. */
    uint8x16_t lookup = table_lookup_8x16(table[0][i], input);
    /* Mask the lookup so we only keep values that were actually in this range. */
    result = bit_or(result, bit_and(lookup, range_match));
  }
#  endif
  return result;
}

/* emit_mask must contain 0xFF for each byte to emit and 0x00 for the rest.
 * Return the low and high bits in different variables. */
static inline void get_shuffle_indices(uint8x16_t emit_mask, uint8_t &mask_lo, uint8_t &mask_hi)
{
#  if defined(USE_NEON)
  const uint8x16_t mask_comp = make8x16(1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128);
  uint8x16_t mask_vec = bit_and(emit_mask, mask_comp);
  mask_lo = reduce_add(get_low_8x16(mask_vec));
  mask_hi = reduce_add(get_high_8x16(mask_vec));

#  elif defined(USE_SSE4_2)
  uint16_t mask = get_mask(emit_mask);
  mask_lo = uint8_t(mask);
  mask_hi = uint8_t(mask >> 8);
#  endif
}

void load_8x128_table(uint8x16x4_t map_v[2], const uint8_t *src)
{
#  if defined(USE_SSE4_2)
  /* SSE cannot load these in 2 operations. */
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 4; ++i) {
      map_v[j][i] = load8x16_unaligned(src + i * 16 + j * 64);
    }
  }
#  elif defined(USE_NEON)
  map_v[0] = load8x16x4_unaligned(src);
  map_v[1] = load8x16x4_unaligned(src + 64);
#  endif
}

void to_uint32x4x2(uint8x8_t data, uint32x4_t &out_low, uint32x4_t &out_hi)
{
#  if defined(USE_SSE4_2)
  out_low = to_uint32x4(get_low_8x8(data));
  out_hi = to_uint32x4(get_high_8x8(data));
#  elif defined(USE_NEON)
  /* The offsets are contained inside the 8 bit shuffle vector.
   * We need to promote it to 32 bit before adding the base offset. */
  const uint16x8_t data_uint16 = to_uint16x8(data);
  out_low = to_uint32x4(get_low_16x8(data_uint16));
  out_hi = to_uint32x4(get_high_16x8(data_uint16));
#  endif
}

#endif

void TokenBuffer::tokenize(const CharClass char_class_table[128])
{
  uint32_t offset = 0, cursor = 0;

  const uint8_t *str = (const uint8_t *)str_.data();

#if defined(USE_SSE4_2) || defined(USE_NEON)
  uint8x16x4_t map_v[2];
  load_8x128_table(map_v, (const uint8_t *)char_class_table);

  const uint8x16_t mask_last = make8x16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF);
  const uint8x16_t to_type_threshold = make8x16(uint8_t(CharClass::ClassToTypeThreshold));
  const uint8x16_t can_merge = make8x16(uint8_t(CharClass::CanMerge));

  uint8x16_t prev = make8x16(uint8_t(CharClass::None));

  for (; offset + 16 <= str_.size(); offset += 16) {
    /* Load 16 chars. */
    const uint8x16_t c = load8x16_unaligned(str + offset);
    /* Lookup their class. */
    const uint8x16_t curr = simd_transform16_ascii(map_v, c);
    /* (curr > ClassToTypeThreshold) ? curr : c */
    const uint8x16_t type = byte_select(c, curr, greater_than(curr, to_type_threshold));
    /* Add the last iteration end token at the end of the vector. */
    prev = right_shift_by_one_element(byte_select(curr, prev, mask_last));
    /* Equivalent to: `!bool(curr & prev & CanMerge)`. */
    const uint8x16_t emit = equal(bit_and(bit_and(curr, prev), can_merge), zero8x16());

    /* Stream compaction of data based on the emit mask (0xFF == emit, 0x00 == skip).
     * Stores `data` compacted inside `data_out` starting from `data_out + cursor` and advance
     * `cursor` by the number of element compacted. */
    {
      /* Make it 1 bit valid element flag. */
      uint8_t mask_lo, mask_hi;
      get_shuffle_indices(emit, mask_lo, mask_hi);

      auto emit_chunk = [&](uint8_t emit_bit_mask, uint8x8_t data, uint32_t base_offset) {
        /* Lookup the shuffle vector. */
        uint8x8_t shuffle = load8x8_unaligned(shuffle_table_8[emit_bit_mask]);
        /* Move data to destination elements (compaction). */
        uint8x8_t data_packed = table_lookup_8x8(data, shuffle);
        /* Write 8 types in the stream. */
        store8x8_unaligned((uint8_t *)types_.get() + cursor, data_packed);

        /* The offsets are contained inside the 8 bit shuffle vector.
         * We need to promote it to 32 bit before adding the base offset. */
        uint32x4_t shuffle32_lo, shuffle32_hi;
        to_uint32x4x2(shuffle, shuffle32_lo, shuffle32_hi);
        const uint32x4_t offset_vec = make32x4(base_offset);
        /* Write 8 offsets. */
        store32x4_unaligned(offsets_.get() + cursor + 0, add(shuffle32_lo, offset_vec));
        store32x4_unaligned(offsets_.get() + cursor + 4, add(shuffle32_hi, offset_vec));
        cursor += count_bits_i(emit_bit_mask);
      };

      emit_chunk(mask_lo, get_low_8x16(type), offset + 0);
      emit_chunk(mask_hi, get_high_8x16(type), offset + 8);
    }

    prev = curr;
  }
  /* Finish tail using scalar loop. */
  const CharClass last_type = CharClass(get_end_lane(prev));
#else

  /* Scalar only implementation. */
  CharClass last_type = CharClass::None;
#endif

  {
    CharClass prev = last_type;
    for (; offset < str_.size(); offset += 1) {
      const char c = str_[offset];
      const CharClass curr = char_class_table[c];
      /* It is faster to overwrite the previous value with the same value
       * as having a condition. */
      types_[cursor] = (curr > CharClass::ClassToTypeThreshold) ? TokenType(curr) : TokenType(c);
      offsets_[cursor] = offset;
      /* Split if no class in common. */
      cursor += (uint8_t(curr) & uint8_t(prev) & uint8_t(CharClass::CanMerge)) == 0;
      prev = curr;
    }
  }

  /* Set end of last token. */
  offsets_[cursor] = str_.size();
  /* Set end of file token. */
  types_[cursor] = EndOfFile;

  size_ = cursor;
  whitespaces_collapsed_ = false;
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
