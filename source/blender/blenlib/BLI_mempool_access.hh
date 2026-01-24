/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Provide minimal access to #BLI_mempool memory chunks.
 * While this should generally be avoided, it's needed to implement
 * #BLI_task_parallel_mempool_chunks.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* NOTE: copied from BLO_blend_defs.h, don't use here because we're in BLI. */
#ifdef __BIG_ENDIAN__
/* Big Endian */
#  define _BLI_MEMPOOL_MAKE_ID_4(a, b, c, d) ((int)(a) << 24 | (int)(b) << 16 | (c) << 8 | (d))
#  define _BLI_MEMPOOL_MAKE_ID_8(a, b, c, d, e, f, g, h) \
    ((int64_t)(a) << 56 | (int64_t)(b) << 48 | (int64_t)(c) << 40 | (int64_t)(d) << 32 | \
     (int64_t)(e) << 24 | (int64_t)(f) << 16 | (int64_t)(g) << 8 | (h))
#else
/* Little Endian */
#  define _BLI_MEMPOOL_MAKE_ID_4(a, b, c, d) ((int)(d) << 24 | (int)(c) << 16 | (b) << 8 | (a))
#  define _BLI_MEMPOOL_MAKE_ID_8(a, b, c, d, e, f, g, h) \
    ((int64_t)(h) << 56 | (int64_t)(g) << 48 | (int64_t)(f) << 40 | (int64_t)(e) << 32 | \
     (int64_t)(d) << 24 | (int64_t)(c) << 16 | (int64_t)(b) << 8 | (a))
#endif

/**
 * Important that this value is _not_ aligned with `sizeof(void *)`.
 * So having a pointer to 2/4/8... aligned memory is enough to ensure
 * the `freeword` will never be used.
 * To be safe, use a word that's the same in both directions.
 */
#define _BLI_MEMPOOL_FREEWORD \
  ((sizeof(void *) > sizeof(int32_t)) ? \
       _BLI_MEMPOOL_MAKE_ID_8('e', 'e', 'r', 'f', 'f', 'r', 'e', 'e') : \
       _BLI_MEMPOOL_MAKE_ID_4('e', 'f', 'f', 'e'))

#define _BLI_MEMPOOL_ELEM_IS_FREE_IMPL(elem) \
  (((const intptr_t *)(elem))[1] == _BLI_MEMPOOL_FREEWORD)

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace blender {
/**
 * Check if a mempool element is free.
 * In ASAN/Valgrind builds this handles unpoisoning/repoisoning memory.
 */
bool _BLI_mempool_elem_is_free_debug(const void *elem);
}  // namespace blender

#  if defined(WITH_ASAN) || defined(WITH_MEM_VALGRIND)
#    define _BLI_MEMPOOL_ELEM_IS_FREE(elem) ::blender::_BLI_mempool_elem_is_free_debug(elem)
#  else
#    define _BLI_MEMPOOL_ELEM_IS_FREE(elem) _BLI_MEMPOOL_ELEM_IS_FREE_IMPL(elem)
#  endif
#endif
