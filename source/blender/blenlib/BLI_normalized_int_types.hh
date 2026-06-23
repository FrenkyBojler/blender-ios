/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include "BLI_math_vector_types.hh"

namespace blender {

template<typename T, int x_sz, int y_sz, int z_sz, int w_sz> struct NormalizedInt4Packed {
  using VecT = VecBase<T, 4>;
  constexpr static bool is_signed = std::is_signed<T>();
  constexpr static T x_max = (1 << (x_sz - int(is_signed))) - 1;
  constexpr static T y_max = (1 << (y_sz - int(is_signed))) - 1;
  constexpr static T z_max = (1 << (z_sz - int(is_signed))) - 1;
  constexpr static T w_max = (1 << (w_sz - int(is_signed))) - 1;

  T x : x_sz;
  T y : y_sz;
  T z : z_sz;
  T w : w_sz;

  NormalizedInt4Packed() = default;

  NormalizedInt4Packed(float4 value)
  {
#if 0 /* Standard compliant version. */
    x = std::clamp(std::round(value.x * x_max), float(is_signed ? -x_max : 0), float(x_max));
    y = std::clamp(std::round(value.y * y_max), float(is_signed ? -y_max : 0), float(y_max));
    z = std::clamp(std::round(value.z * z_max), float(is_signed ? -z_max : 0), float(z_max));
    w = std::clamp(std::round(value.w * w_max), float(is_signed ? -w_max : 0), float(w_max));
#else /* Faster but bug-prone. */
    x = value.x * x_max;
    y = value.y * y_max;
    z = value.z * z_max;
    w = value.w * w_max;
#endif
  }

  NormalizedInt4Packed(VecT value)
  {
    x = value.x;
    y = value.y;
    z = value.z;
    w = value.w;
  }

  operator float4() const
  {
    return float4(VecT(*this)) / float4(x_max, y_max, z_max, w_max);
  }

  operator VecT() const
  {
    return VecT(x, y, z, w);
  }
};

template<typename T, int x_sz, int y_sz> struct NormalizedInt2Packed {
  using VecT = VecBase<T, 2>;
  constexpr static bool is_signed = std::is_signed<T>();
  constexpr static T x_max = (1 << (x_sz - int(is_signed))) - 1;
  constexpr static T y_max = (1 << (y_sz - int(is_signed))) - 1;

  T x : x_sz;
  T y : y_sz;

  NormalizedInt2Packed() = default;

  NormalizedInt2Packed(float2 value)
  {
#if 0 /* Standard compliant version. */
    x = std::clamp(std::round(value.x * x_max), float(is_signed ? -x_max : 0), float(x_max));
    y = std::clamp(std::round(value.y * y_max), float(is_signed ? -y_max : 0), float(y_max));
#else /* Faster but bug-prone. */
    x = value.x * x_max;
    y = value.y * y_max;
#endif
  }

  NormalizedInt2Packed(VecT value)
  {
    x = value.x;
    y = value.y;
  }

  operator float2() const
  {
    return float2(VecT(*this)) / float2(x_max, y_max);
  }

  operator VecT() const
  {
    return VecT(x, y);
  }
};

using char4_norm = NormalizedInt4Packed<int32_t, 8, 8, 8, 8>;
using uchar4_norm = NormalizedInt4Packed<uint32_t, 8, 8, 8, 8>;
using short2_norm = NormalizedInt2Packed<int32_t, 16, 16>;
using ushort2_norm = NormalizedInt2Packed<uint32_t, 16, 16>;
using short4_norm = NormalizedInt4Packed<int32_t, 16, 16, 16, 16>;
using ushort4_norm = NormalizedInt4Packed<uint32_t, 16, 16, 16, 16>;
using int1010102_norm = NormalizedInt4Packed<int32_t, 10, 10, 10, 2>;
using uint1010102_norm = NormalizedInt4Packed<uint32_t, 10, 10, 10, 2>;

}  // namespace blender
