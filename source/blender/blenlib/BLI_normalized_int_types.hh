/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include "BLI_math_vector.hh"

namespace blender {

namespace detail {
template<typename T, int SIZE, int... ITEM_SIZE> struct NormalizedIntVec;

template<typename T, int X_SIZE, int Y_SIZE> struct NormalizedIntVec<T, 2, X_SIZE, Y_SIZE> {
  using VecT = VecBase<float, 2>;
  using IntVecT = VecBase<T, 2>;
  constexpr static bool is_signed = std::is_signed<T>();
  constexpr static T x_max = (1 << (X_SIZE - int(is_signed))) - 1;
  constexpr static T y_max = (1 << (Y_SIZE - int(is_signed))) - 1;

  T x : X_SIZE;
  T y : Y_SIZE;

  NormalizedIntVec() = default;
  constexpr NormalizedIntVec(IntVecT value) : x(value.x), y(value.y) {}

  operator IntVecT() const
  {
    return IntVecT(x, y);
  }

  static constexpr VecT max()
  {
    return VecT(x_max, y_max);
  }

  static constexpr VecT min()
  {
    if (is_signed) {
      return VecT(-x_max, -y_max);
    }
    return VecT(0.0f, 0.0f);
  }
};

template<typename T, int X_SIZE, int Y_SIZE, int Z_SIZE, int W_SIZE>
struct NormalizedIntVec<T, 4, X_SIZE, Y_SIZE, Z_SIZE, W_SIZE> {
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

  NormalizedIntVec() = default;
  constexpr NormalizedIntVec(IntVecT value) : x(value.x), y(value.y), z(value.z), w(value.w) {}

  operator IntVecT() const
  {
    return IntVecT(x, y, z, w);
  }

  static constexpr VecT max()
  {
    return VecT(x_max, y_max, z_max, w_max);
  }

  static constexpr VecT min()
  {
    if constexpr (is_signed) {
      return VecT(-x_max, -y_max, -z_max, -w_max);
    }
    return VecT(0.0f, 0.0f, 0.0f, 0.0f);
  }
};

}  // namespace detail

template<typename T, int SIZE, int... ITEM_SIZE>
  requires(std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>)
struct NormalizedIntVecBase : detail::NormalizedIntVec<T, SIZE, ITEM_SIZE...> {
  using IntPacked = detail::NormalizedIntVec<T, SIZE, ITEM_SIZE...>;
  using typename IntPacked::IntVecT;
  using typename IntPacked::VecT;

  NormalizedIntVecBase() = default;

  NormalizedIntVecBase(IntVecT value) : IntPacked(value) {}

  /* Adding rounding would be the standard compliant conversion.
   * But this would introduce perf regression. */
  NormalizedIntVecBase(VecT val)
      : IntPacked(IntVecT(math::clamp(val * IntPacked::max(), IntPacked::min(), IntPacked::max())))
  {
  }

  operator VecT() const
  {
    return VecT(IntVecT(*this)) / IntPacked::max();
  }
};

using char4_norm = NormalizedIntVecBase<int32_t, 4, 8, 8, 8, 8>;
using uchar4_norm = NormalizedIntVecBase<uint32_t, 4, 8, 8, 8, 8>;
using short2_norm = NormalizedIntVecBase<int32_t, 2, 16, 16>;
using ushort2_norm = NormalizedIntVecBase<uint32_t, 2, 16, 16>;
using short4_norm = NormalizedIntVecBase<int32_t, 4, 16, 16, 16, 16>;
using ushort4_norm = NormalizedIntVecBase<uint32_t, 4, 16, 16, 16, 16>;
using int1010102_norm = NormalizedIntVecBase<int32_t, 4, 10, 10, 10, 2>;
using uint1010102_norm = NormalizedIntVecBase<uint32_t, 4, 10, 10, 10, 2>;

}  // namespace blender
