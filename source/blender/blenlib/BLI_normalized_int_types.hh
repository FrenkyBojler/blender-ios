/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include "BLI_math_vector_types.hh"

namespace blender {

namespace detail {
template<typename T, int SIZE, int... ITEM_SIZE> struct IntPack;

template<typename T, int X_SIZE, int Y_SIZE> struct IntPack<T, 2, X_SIZE, Y_SIZE> {
  using VecT = VecBase<float, 2>;
  using IntVecT = VecBase<T, 2>;
  constexpr static bool is_signed = std::is_signed<T>();
  constexpr static T x_max = (1 << (X_SIZE - int(is_signed))) - 1;
  constexpr static T y_max = (1 << (Y_SIZE - int(is_signed))) - 1;

  T x : X_SIZE;
  T y : Y_SIZE;

  operator VecT() const
  {
    return VecT(IntVecT(*this)) / VecT(x_max, y_max);
  }

  operator IntVecT() const
  {
    return IntVecT(x, y);
  }
};

template<typename T, int X_SIZE, int Y_SIZE, int Z_SIZE, int W_SIZE>
struct IntPack<T, 4, X_SIZE, Y_SIZE, Z_SIZE, W_SIZE> {
  using VecT = VecBase<float, 4>;
  using IntVecT = VecBase<T, 4>;
  constexpr static bool is_signed = std::is_signed<T>();
  constexpr static T x_max = (1 << (X_SIZE - int(is_signed))) - 1;
  constexpr static T y_max = (1 << (Y_SIZE - int(is_signed))) - 1;
  constexpr static T z_max = (1 << (Z_SIZE - int(is_signed))) - 1;
  constexpr static T w_max = (1 << (W_SIZE - int(is_signed))) - 1;

  T x : X_SIZE;
  T y : Y_SIZE;
  T z : Z_SIZE;
  T w : W_SIZE;

  operator VecT() const
  {
    return VecT(IntVecT(*this)) / VecT(x_max, y_max, z_max, w_max);
  }

  operator IntVecT() const
  {
    return IntVecT(x, y, z, w);
  }
};

}  // namespace detail

template<typename T, int SIZE, int... ITEM_SIZE>
  requires(std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>)
struct NormalizedIntPacked : detail::IntPack<T, SIZE, ITEM_SIZE...> {
  using VecT = VecBase<float, SIZE>;
  using IntVecT = VecBase<T, SIZE>;

  NormalizedIntPacked() = default;

  NormalizedIntPacked(VecT value)
  {
    this->x = rescale_value(value.x, this->x_max);
    if constexpr (SIZE > 1) {
      this->y = rescale_value(value.y, this->y_max);
    }
    if constexpr (SIZE > 2) {
      this->z = rescale_value(value.z, this->z_max);
    }
    if constexpr (SIZE > 3) {
      this->w = rescale_value(value.w, this->w_max);
    }
  }

  NormalizedIntPacked(IntVecT value)
  {
    this->x = value.x;
    if constexpr (SIZE > 1) {
      this->y = value.y;
    }
    if constexpr (SIZE > 2) {
      this->z = value.z;
    }
    if constexpr (SIZE > 3) {
      this->w = value.w;
    }
  }

 private:
  T rescale_value(float val, T max)
  {
    val *= max;
#if 0 /* That would be the standard compliant conversion. But this introduce perf regression. */
    val = std::round(val);
#endif
    return std::clamp(val, float(this->is_signed ? -max : 0), float(max));
  }
};

using char4_norm = NormalizedIntPacked<int32_t, 4, 8, 8, 8, 8>;
using uchar4_norm = NormalizedIntPacked<uint32_t, 4, 8, 8, 8, 8>;
using short2_norm = NormalizedIntPacked<int32_t, 2, 16, 16>;
using ushort2_norm = NormalizedIntPacked<uint32_t, 2, 16, 16>;
using short4_norm = NormalizedIntPacked<int32_t, 4, 16, 16, 16, 16>;
using ushort4_norm = NormalizedIntPacked<uint32_t, 4, 16, 16, 16, 16>;
using int1010102_norm = NormalizedIntPacked<int32_t, 4, 10, 10, 10, 2>;
using uint1010102_norm = NormalizedIntPacked<uint32_t, 4, 10, 10, 10, 2>;

}  // namespace blender
