/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Defines:
 * - #BLI_assume_assert
 */

#include <stddef.h>
#include "BLI_assert.h"

/* _BLI_ASSUME */
#  if defined(__GNUC__)
#    define _BLI_ASSUME(a) assume(a)
#  elif defined(__clang__)
#    define _BLI_ASSUME(a) __builtin_assume( [&]() __attribute__((pure)) { return bool(a); }())
#  elif defined(_MSC_VER)
#    define _BLI_ASSUME(a) __assume(a)
#  else
#    define _BLI_ASSUME(a) ((void)0)
#  endif

#define BLI_assume_assert(a) (BLI_assert(a), _BLI_ASSUME(a))