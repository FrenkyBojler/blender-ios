/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: MIT */

/**
 * Small SIMD library to avoid duplicating code.
 */

#pragma once

#if defined(__ARM_NEON)
#  define USE_NEON
#  include <arm_neon.h>
#endif

#if (defined(__x86_64__) || defined(_M_X64)) && defined(__SSE4_2__)
#  define USE_SSE4_2
#  include <immintrin.h>
#endif

#if defined(USE_NEON) || defined(USE_SSE4_2)

#  ifdef USE_SSE4_2
using uint8x16_t = __m128i;
using uint16x8_t = __m128i;
using uint32x4_t = __m128i;
using uint8x8_t = __m128i; /* Only carry data in the first 8 bytes. */
using uint8x4_t = __m128i; /* Only carry data in the first 4 bytes. */
using uint8x16x4_t = __m128i[4];
#  endif

#  if defined(USE_NEON)
/* NEON: Overflow returns 0.
 * SSSE3: Doesn't have an equivalent. */
static inline uint8x16_t table_lookup_8x16x4(const uint8x16x4_t table, const uint8x16_t input)
{
  return vqtbl4q_u8(table, input);
}
#  endif

/* NEON: Overflow returns 0.
 * SSSE3: Overflow will wrap around. */
static inline uint8x16_t table_lookup_8x16(const uint8x16_t table, const uint8x16_t input)
{
#  if defined(USE_NEON)
  return vqtbl1q_u8(table, input);
#  elif defined(USE_SSE4_2)
  return _mm_shuffle_epi8(table, input);
#  endif
}

static inline uint8x8_t table_lookup_8x8(const uint8x8_t table, const uint8x8_t input)
{
#  if defined(USE_NEON)
  return vtbl1_u8(table, input);
#  elif defined(USE_SSE4_2)
  return _mm_shuffle_epi8(table, input);
#  endif
}

static inline uint8x16_t equal(const uint8x16_t a, const uint8x16_t b)
{
#  if defined(USE_NEON)
  return vceqq_u8(a, b);
#  elif defined(USE_SSE4_2)
  return _mm_cmpeq_epi8(a, b);
#  endif
}

static inline uint8x16_t greater_than(const uint8x16_t a, const uint8x16_t b)
{
#  if defined(USE_NEON)
  return vcgtq_u8(a, b);
#  elif defined(USE_SSE4_2)
  return _mm_cmpgt_epi8(a, b);
#  endif
}

#  if defined(USE_NEON)
static inline uint8_t reduce_add(const uint8x8_t input)
{
  return vaddv_u8(input);
}
#  endif

#  ifdef USE_SSE4_2
/* Return the MSB of each element */
static inline uint16_t get_mask(const uint8x16_t a)
{
  return _mm_movemask_epi8(a);
}
#  endif

/* Note: SSE version is a NOOP since uint8x8_t is still 128bits. */
static inline uint8x8_t get_low_8x16(const uint8x16_t a)
{
#  if defined(USE_NEON)
  return vget_low_u8(a);
#  elif defined(USE_SSE4_2)
  return a;
#  endif
}

static inline uint8x8_t get_high_8x16(const uint8x16_t a)
{
#  if defined(USE_NEON)
  return vget_high_u8(a);
#  elif defined(USE_SSE4_2)
  /* Right shift by 8 bytes. */
  return _mm_srli_si128(a, 8);
#  endif
}

static inline uint8_t get_end_lane(const uint8x16_t a)
{
#  if defined(USE_NEON)
  return vgetq_lane_u8(a, 15);
#  elif defined(USE_SSE4_2)
  return _mm_extract_epi8(a, 15);
#  endif
}

#  if defined(USE_NEON)
static inline uint16x4_t get_low_16x8(const uint16x8_t a)
{
  return vget_low_u16(a);
}
static inline uint16x4_t get_high_16x8(const uint16x8_t a)
{
  return vget_high_u16(a);
}

static inline uint16x8_t to_uint16x8(const uint8x8_t a)
{
  return vmovl_u8(a);
}
static inline uint32x4_t to_uint32x4(const uint16x4_t a)
{
  return vmovl_u16(a);
}

#  elif defined(USE_SSE4_2)
static inline uint8x4_t get_low_8x8(const uint8x8_t a)
{
  return a;
}
static inline uint8x4_t get_high_8x8(const uint8x8_t a)
{
  return _mm_srli_si128(a, 4);
}

static inline uint32x4_t to_uint32x4(const uint8x4_t a)
{
  return _mm_cvtepu8_epi32(a);
}
#  endif

static inline uint8x16_t bit_and(const uint8x16_t a, const uint8x16_t b)
{
#  if defined(USE_NEON)
  return vandq_u8(a, b);
#  elif defined(USE_SSE4_2)
  return _mm_and_si128(a, b);
#  endif
}

static inline uint8x16_t bit_or(const uint8x16_t a, const uint8x16_t b)
{
#  if defined(USE_NEON)
  return vorrq_u8(a, b);
#  elif defined(USE_SSE4_2)
  return _mm_or_si128(a, b);
#  endif
}

static inline uint8x16_t bit_xor(const uint8x16_t a, const uint8x16_t b)
{
#  if defined(USE_NEON)
  return veorq_u8(a, b);
#  elif defined(USE_SSE4_2)
  return _mm_xor_si128(a, b);
#  endif
}

static inline uint32x4_t add(const uint32x4_t a, const uint32x4_t b)
{
#  if defined(USE_NEON)
  return vaddq_u32(a, b);
#  elif defined(USE_SSE4_2)
  return _mm_add_epi32(a, b);
#  endif
}

/* Only valid if mask contains 0x00 or 0xFF. */
static inline uint8x16_t byte_select(const uint8x16_t a, const uint8x16_t b, const uint8x16_t mask)
{
#  if defined(USE_NEON)
  return vbslq_u8(mask, b, a);
#  elif defined(USE_SSE4_2)
  return _mm_blendv_epi8(a, b, mask);
#  endif
}

/* Shift all elements (bytes) by one element to the right with wrap-around. */
static inline uint8x16_t right_shift_by_one_element(const uint8x16_t a)
{
#  if defined(USE_NEON)
  return vextq_u8(a, a, 15);
#  elif defined(USE_SSE4_2)
  return _mm_alignr_epi8(a, a, 15);
#  endif
}

static inline uint8x16_t zero8x16()
{
#  if defined(USE_NEON)
  return vdupq_n_u8(0);
#  elif defined(USE_SSE4_2)
  return _mm_setzero_si128();
#  endif
}

static inline uint8x16_t make8x16(const uint8_t a)
{
#  if defined(USE_NEON)
  return vdupq_n_u8(a);
#  elif defined(USE_SSE4_2)
  return _mm_set1_epi8(a);
#  endif
}

static inline uint32x4_t make32x4(const uint32_t a)
{
#  if defined(USE_NEON)
  return vdupq_n_u32(a);
#  elif defined(USE_SSE4_2)
  return _mm_set1_epi32(a);
#  endif
}

static inline uint8x16_t make8x16(const uint8_t a0,
                                  const uint8_t a1,
                                  const uint8_t a2,
                                  const uint8_t a3,
                                  const uint8_t a4,
                                  const uint8_t a5,
                                  const uint8_t a6,
                                  const uint8_t a7,
                                  const uint8_t a8,
                                  const uint8_t a9,
                                  const uint8_t a10,
                                  const uint8_t a11,
                                  const uint8_t a12,
                                  const uint8_t a13,
                                  const uint8_t a14,
                                  const uint8_t a15)
{

#  if defined(USE_NEON)
  return {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15};
#  elif defined(USE_SSE4_2)
  return _mm_set_epi8(a15, a14, a13, a12, a11, a10, a9, a8, a7, a6, a5, a4, a3, a2, a1, a0);
#  endif
}

static inline uint8x16_t load8x16_unaligned(const uint8_t *a)
{
#  if defined(USE_NEON)
  return vld1q_u8(a);
#  elif defined(USE_SSE4_2)
  return _mm_loadu_si128((const __m128i *)a);
#  endif
}

static inline uint8x16_t load8x16_aligned(const uint8_t *a)
{
#  if defined(USE_NEON)
  return vld1q_u8(a);
#  elif defined(USE_SSE4_2)
  return _mm_load_si128((const __m128i *)a);
#  endif
}

static inline uint8x8_t load8x8_unaligned(const uint8_t *a)
{
#  if defined(USE_NEON)
  return vld1_u8(a);
#  elif defined(USE_SSE4_2)
  return _mm_loadu_si64((const __m128i *)a);
#  endif
}

#  if defined(USE_NEON)
static inline uint8x16x4_t load8x16x4_unaligned(const uint8_t *a)
{
  return vld1q_u8_x4(a);
}
#  endif

static inline void store8x8_unaligned(uint8_t *dst, const uint8x8_t src)
{
#  if defined(USE_NEON)
  vst1_u8(dst, src);
#  elif defined(USE_SSE4_2)
  return _mm_storeu_si64(dst, src);
#  endif
}

static inline void store32x4_unaligned(uint32_t *dst, const uint32x4_t src)
{
#  if defined(USE_NEON)
  vst1q_u32(dst, src);
#  elif defined(USE_SSE4_2)
  return _mm_storeu_si128((__m128i *)dst, src);
#  endif
}

static inline void store32x4_aligned(uint32_t *dst, const uint32x4_t src)
{
#  if defined(USE_NEON)
  vst1q_u32(dst, src);
#  elif defined(USE_SSE4_2)
  return _mm_store_si128((__m128i *)dst, src);
#  endif
}

#endif
