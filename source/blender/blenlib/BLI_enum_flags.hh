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
 * `_max_enum_value` needs to be the largest individual bit value present in the enum. */
#  define ENUM_OPERATORS(_enum_type, _max_enum_value) \
    inline constexpr _enum_type operator|(_enum_type a, _enum_type b) \
    { \
      return (_enum_type)(uint64_t(a) | uint64_t(b)); \
    } \
    inline constexpr _enum_type operator&(_enum_type a, _enum_type b) \
    { \
      return (_enum_type)(uint64_t(a) & uint64_t(b)); \
    } \
    inline constexpr _enum_type operator~(_enum_type a) \
    { \
      return (_enum_type)(~uint64_t(a) & (2 * uint64_t(_max_enum_value) - 1)); \
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

#  define ENUM_OPERATORS(_enum_type, _max_enum_value)

#endif
