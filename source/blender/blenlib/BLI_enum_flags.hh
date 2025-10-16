/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#ifdef __cplusplus
#  include <cstdint>

/* Use for enum classes that represent bit flags.
 * Defines logical operators to combine and mask the flag values.
 *
 * Note that negation/inversion operator (~) flips all the bits, so the result can contain
 * set bits that are not part of the enum values. However that is fine in typical
 * inversion operator usage, which is often for masking out bits (`a & ~b`). */
#  define ENUM_OPERATORS(_enum_type) \
    [[nodiscard]] inline constexpr _enum_type operator|(_enum_type a, _enum_type b) \
    { \
      return (_enum_type)(uint64_t(a) | uint64_t(b)); \
    } \
    [[nodiscard]] inline constexpr _enum_type operator&(_enum_type a, _enum_type b) \
    { \
      return (_enum_type)(uint64_t(a) & uint64_t(b)); \
    } \
    [[nodiscard]] inline constexpr _enum_type operator~(_enum_type a) \
    { \
      return (_enum_type)(~uint64_t(a)); \
    } \
    inline _enum_type &operator|=(_enum_type &a, _enum_type b) \
    { \
      return a = (_enum_type)(uint64_t(a) | uint64_t(b)); \
    } \
    inline _enum_type &operator&=(_enum_type &a, _enum_type b) \
    { \
      return a = (_enum_type)(uint64_t(a) & uint64_t(b)); \
    } \
    inline _enum_type &operator^=(_enum_type &a, _enum_type b) \
    { \
      return a = (_enum_type)(uint64_t(a) ^ uint64_t(b)); \
    }

#else

#  define ENUM_OPERATORS(_enum_type)

#endif
