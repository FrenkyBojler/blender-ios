/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include "BLI_assert.h"

#include <array>
#include <type_traits>

namespace blender {

/* Needed forward declaration to allow the swizzle to reference it. */
template<typename T, int Size> struct VecBase;

/**
 * Swizzle functions for vector of non-trivial types.
 * Does not support assignment.
 */
template<typename T, int Size> struct VecSwizzleFunc {};

template<typename T> struct VecSwizzleFunc<T, 2> {
  [[nodiscard]] VecBase<T, 2> xy() const
  {
    const VecBase<T, 2> &vec = *reinterpret_cast<const VecBase<T, 2> *>(this);
    return {vec.x, vec.y};
  }
};

template<typename T> struct VecSwizzleFunc<T, 3> : VecSwizzleFunc<T, 2> {
  [[nodiscard]] VecBase<T, 3> xyz() const
  {
    const VecBase<T, 3> &vec = *reinterpret_cast<const VecBase<T, 3> *>(this);
    return {vec.x, vec.y, vec.z};
  }

  [[nodiscard]] VecBase<T, 2> yz() const
  {
    const VecBase<T, 3> &vec = *reinterpret_cast<const VecBase<T, 3> *>(this);
    return {vec.y, vec.z};
  }
};

template<typename T> struct VecSwizzleFunc<T, 4> : VecSwizzleFunc<T, 2> {
  [[nodiscard]] VecBase<T, 2> zw() const
  {
    const VecBase<T, 4> &vec = *reinterpret_cast<const VecBase<T, 4> *>(this);
    return {vec.z, vec.w};
  }

  [[nodiscard]] VecBase<T, 3> yzw() const
  {
    const VecBase<T, 4> &vec = *reinterpret_cast<const VecBase<T, 4> *>(this);
    return {vec.y, vec.z, vec.w};
  }

  [[nodiscard]] VecBase<T, 4> xyzw() const
  {
    const VecBase<T, 4> &vec = *reinterpret_cast<const VecBase<T, 4> *>(this);
    return {vec.x, vec.y, vec.z, vec.w};
  }
};

/**
 * Swizzle class that supports reordering of component.
 * Will decay to a vector of the same size.
 * Does not support assignment (but is still copy & move constructible, see below).
 *
 * IMPORTANT: Must be declared at the same memory location as the first component referenced in the
 * swizzle. So for `zzwy` it is `y`. We do this to allow the copy constructor to copy only the
 * referenced component in the case of something like `a.zzy = b.zzy`. This is because we do not
 * want to override (or delete) the copy constructor as it would make the vector types non-trivial.
 */
template<typename T, int Size, int x, int y, int z = y, int w = z> struct VecSwizzleReadOnly {
  using VecT = VecBase<T, Size>;
  static constexpr int max_comp = std::max(std::max(std::max(x, y), z), w);
  static constexpr int min_comp = std::min(std::min(std::min(x, y), z), w);
  static constexpr int effective_len = max_comp - min_comp + 1;

 private:
  std::array<T, effective_len> values_;

 public:
  VecSwizzleReadOnly() = default;

  operator VecT() const
  {
    /* Can only do this when VecT has been instantiated. */
    BLI_STATIC_ASSERT(alignof(VecT) <= alignof(T),
                      "VecSwizzleReadOnly is not compatible with aligned type for now.");
    BLI_STATIC_ASSERT(std::is_trivial_v<VecT>, "Can only swizzle trivial vectors.");
    BLI_STATIC_ASSERT(Size >= 2 && Size <= 4, "Only small vector supports swizzles");
    if constexpr (Size == 4) {
      return {values_[x - min_comp],
              values_[y - min_comp],
              values_[z - min_comp],
              values_[w - min_comp]};
    }
    else if constexpr (Size == 3) {
      return {values_[x - min_comp], values_[y - min_comp], values_[z - min_comp]};
    }
    else if constexpr (Size == 2) {
      return {values_[x - min_comp], values_[y - min_comp]};
    }
    return {};
  }

  [[nodiscard]] VecT operator()() const
  {
    return VecT(*this);
  }
};

/**
 * Swizzle class that supports assignment. Does not support reordering of component.
 * Will decay to a vector of the same size.
 * Can only be used if all swizzled components points to different memory locations (no `xxyy`).
 *
 * IMPORTANT: Must be declared at the same memory location as the first component referenced in the
 * swizzle. So for `yz` it is `y`. We do this to allow the copy constructor to copy only the
 * referenced component in the case of something like `a.zzy = b.zzy`. This is because we do not
 * want to override (or delete) the copy constructor as it would make the vector types non-trivial.
 */
template<typename T, int Size> struct VecSwizzleReadWrite {
  using VecT = VecBase<T, Size>;

 private:
  std::array<T, Size> values_;

 public:
  operator VecT() const
  {
    /* Can only do this when VecT has been instantiated. */
    BLI_STATIC_ASSERT(alignof(VecT) <= alignof(T),
                      "VecSwizzleReadWrite is not compatible with aligned type for now.");
    BLI_STATIC_ASSERT(std::is_trivial_v<VecT>, "Can only swizzle trivial vectors.");
    BLI_STATIC_ASSERT(Size >= 2 && Size <= 4, "Only small vector supports swizzles");
    if constexpr (Size == 4) {
      return {values_[0], values_[1], values_[2], values_[3]};
    }
    else if constexpr (Size == 3) {
      return {values_[0], values_[1], values_[2]};
    }
    else if constexpr (Size == 2) {
      return {values_[0], values_[1]};
    }
    return {};
  }

  [[nodiscard]] VecT operator()() const
  {
    return VecT(*this);
  }

  VecSwizzleReadWrite &operator=(const VecT &other)
  {
    return (*this = *reinterpret_cast<const VecSwizzleReadWrite *>(&other));
  }

  template<int x_, int y_, int z_, int w_>
  VecSwizzleReadWrite &operator=(const VecSwizzleReadOnly<T, Size, x_, y_, z_, w_> &other)
  {
    return (*this = other.operator VecT());
  }

#define IMPL_BINARY(op) \
  { \
    return {*this = VecT(*this) op a}; \
  }
#define IMPL_UNARY(op) \
  { \
    return op VecT(*this); \
  }

#define STD_OP \
  template<typename U = T, typename std::enable_if_t<!std::is_same_v<bool, U>> * = nullptr>

  STD_OP VecT operator+() const IMPL_UNARY(+);
  STD_OP VecT operator-() const IMPL_UNARY(-);

  STD_OP VecSwizzleReadWrite &operator+=(const VecT &a) IMPL_BINARY(+);
  STD_OP VecSwizzleReadWrite &operator-=(const VecT &a) IMPL_BINARY(-);
  STD_OP VecSwizzleReadWrite &operator/=(const VecT &a) IMPL_BINARY(/);
  STD_OP VecSwizzleReadWrite &operator*=(const VecT &a) IMPL_BINARY(*);

  STD_OP VecSwizzleReadWrite &operator+=(const T &a) IMPL_BINARY(+);
  STD_OP VecSwizzleReadWrite &operator-=(const T &a) IMPL_BINARY(-);
  STD_OP VecSwizzleReadWrite &operator/=(const T &a) IMPL_BINARY(/);
  STD_OP VecSwizzleReadWrite &operator*=(const T &a) IMPL_BINARY(*);

#define INT_OP \
  template<typename U = T, \
           typename std::enable_if_t<std::is_integral_v<U>> * = nullptr, \
           typename std::enable_if_t<!std::is_same_v<bool, U>> * = nullptr>

  INT_OP VecT operator~() const IMPL_UNARY(~);

  INT_OP VecSwizzleReadWrite &operator%=(const VecT &a) IMPL_BINARY(%);
  INT_OP VecSwizzleReadWrite &operator&=(const VecT &a) IMPL_BINARY(&);
  INT_OP VecSwizzleReadWrite &operator|=(const VecT &a) IMPL_BINARY(|);
  INT_OP VecSwizzleReadWrite &operator^=(const VecT &a) IMPL_BINARY(^);

  INT_OP VecSwizzleReadWrite &operator%=(const T &a) IMPL_BINARY(%);
  INT_OP VecSwizzleReadWrite &operator&=(const T &a) IMPL_BINARY(&);
  INT_OP VecSwizzleReadWrite &operator|=(const T &a) IMPL_BINARY(|);
  INT_OP VecSwizzleReadWrite &operator^=(const T &a) IMPL_BINARY(^);

  INT_OP VecSwizzleReadWrite &operator<<=(const VecT &a) IMPL_BINARY(<<);
  INT_OP VecSwizzleReadWrite &operator>>=(const VecT &a) IMPL_BINARY(>>);
  INT_OP VecSwizzleReadWrite &operator<<=(const T &a) IMPL_BINARY(<<);
  INT_OP VecSwizzleReadWrite &operator>>=(const T &a) IMPL_BINARY(>>);

#undef INT_OP
#undef IMPL_BINARY
#undef IMPL_UNARY
};

/**
 * List of all swizzles we support.
 * We do not support non-contiguous component swizzle (e.g. xwxw) as they would have undefined
 * behavior in some corner cases.
 * We only generate the variant we use to reduce compile time and binary size.
 * Uncomment at when needed.
 */

/* Swizzles containing X. Must be declared in a union alongside X. */
#define X_SWIZZLES \
  VecSwizzleReadOnly<T, 2, 0, 0> xx, rr; \
  VecSwizzleReadOnly<T, 3, 0, 0, 0> xxx, rrr; \
  VecSwizzleReadOnly<T, 4, 0, 0, 0, 0> xxxx, rrrr;

/* Swizzles containing Y. Must be declared in a union alongside Y. */
#define Y_SWIZZLES \
  VecSwizzleReadOnly<T, 2, 1, 1> yy, gg; \
  VecSwizzleReadOnly<T, 3, 1, 1, 1> yyy, ggg; \
  VecSwizzleReadOnly<T, 4, 1, 1, 1, 1> yyyy, gggg;

/* Swizzles containing Z. Must be declared in a union alongside Z. */
#define Z_SWIZZLES \
  VecSwizzleReadOnly<T, 2, 2, 2> zz, bb; \
  VecSwizzleReadOnly<T, 3, 2, 2, 2> zzz, bbb; \
  VecSwizzleReadOnly<T, 4, 2, 2, 2, 2> zzzz, bbbb;

/* Swizzles containing W. Must be declared in a union alongside W. */
#define W_SWIZZLES \
  VecSwizzleReadOnly<T, 2, 3, 3> ww, aa; \
  VecSwizzleReadOnly<T, 3, 3, 3, 3> www, aaa; \
  VecSwizzleReadOnly<T, 4, 3, 3, 3, 3> wwww, aaaa;

/* Swizzles containing XY. Must be declared in a union alongside X. */
#define XY_SWIZZLES \
  VecSwizzleReadWrite<T, 2> xy, rg; \
  VecSwizzleReadOnly<T, 2, 1, 0> yx, gr; \
  VecSwizzleReadOnly<T, 3, 0, 0, 1> xxy, rrg; \
  VecSwizzleReadOnly<T, 3, 0, 1, 0> xyx, rgr; \
  VecSwizzleReadOnly<T, 3, 0, 1, 1> xyy, rgg; \
  VecSwizzleReadOnly<T, 3, 1, 0, 0> yxx, grr; \
  VecSwizzleReadOnly<T, 3, 1, 0, 1> yxy, grg; \
  VecSwizzleReadOnly<T, 3, 1, 1, 0> yyx, ggr; \
  VecSwizzleReadOnly<T, 4, 0, 0, 0, 1> xxxy, rrrg; \
  VecSwizzleReadOnly<T, 4, 0, 0, 1, 0> xxyx, rrgr; \
  VecSwizzleReadOnly<T, 4, 0, 0, 1, 1> xxyy, rrgg; \
  VecSwizzleReadOnly<T, 4, 0, 1, 0, 0> xyxx, rgrr; \
  VecSwizzleReadOnly<T, 4, 0, 1, 0, 1> xyxy, rgrg; \
  VecSwizzleReadOnly<T, 4, 0, 1, 1, 0> xyyx, rggr; \
  VecSwizzleReadOnly<T, 4, 0, 1, 1, 1> xyyy, rggg; \
  VecSwizzleReadOnly<T, 4, 1, 0, 0, 0> yxxx, grrr; \
  VecSwizzleReadOnly<T, 4, 1, 0, 0, 1> yxxy, grrg; \
  VecSwizzleReadOnly<T, 4, 1, 0, 1, 0> yxyx, grgr; \
  VecSwizzleReadOnly<T, 4, 1, 0, 1, 1> yxyy, grgg; \
  VecSwizzleReadOnly<T, 4, 1, 1, 0, 0> yyxx, ggrr; \
  VecSwizzleReadOnly<T, 4, 1, 1, 0, 1> yyxy, ggrg; \
  VecSwizzleReadOnly<T, 4, 1, 1, 1, 0> yyyx, gggr;

/* Swizzles containing YZ. Must be declared in a union alongside Y. */
#define YZ_SWIZZLES \
  VecSwizzleReadWrite<T, 2> yz, gb; \
  VecSwizzleReadOnly<T, 2, 2, 1> zy, bg; \
  VecSwizzleReadOnly<T, 3, 1, 1, 2> yyz, ggb; \
  VecSwizzleReadOnly<T, 3, 1, 2, 1> yzy, gbg; \
  VecSwizzleReadOnly<T, 3, 1, 2, 2> yzz, gbb; \
  VecSwizzleReadOnly<T, 3, 2, 1, 1> zyy, bgg; \
  VecSwizzleReadOnly<T, 3, 2, 1, 2> zyz, bgb; \
  VecSwizzleReadOnly<T, 3, 2, 2, 1> zzy, bbg; \
  VecSwizzleReadOnly<T, 4, 1, 1, 1, 2> yyyz, gggb; \
  VecSwizzleReadOnly<T, 4, 1, 1, 2, 1> yyzy, ggbg; \
  VecSwizzleReadOnly<T, 4, 1, 1, 2, 2> yyzz, ggbb; \
  VecSwizzleReadOnly<T, 4, 1, 2, 1, 1> yzyy, gbgg; \
  VecSwizzleReadOnly<T, 4, 1, 2, 1, 2> yzyz, gbgb; \
  VecSwizzleReadOnly<T, 4, 1, 2, 2, 1> yzzy, gbbg; \
  VecSwizzleReadOnly<T, 4, 1, 2, 2, 2> yzzz, gbbb; \
  VecSwizzleReadOnly<T, 4, 2, 1, 1, 1> zyyy, bggg; \
  VecSwizzleReadOnly<T, 4, 2, 1, 1, 2> zyyz, bggb; \
  VecSwizzleReadOnly<T, 4, 2, 1, 2, 1> zyzy, bgbg; \
  VecSwizzleReadOnly<T, 4, 2, 1, 2, 2> zyzz, bgbb; \
  VecSwizzleReadOnly<T, 4, 2, 2, 1, 1> zzyy, bbgg; \
  VecSwizzleReadOnly<T, 4, 2, 2, 1, 2> zzyz, bbgb; \
  VecSwizzleReadOnly<T, 4, 2, 2, 2, 1> zzzy, bbbg;

/* Swizzles containing ZW. Must be declared in a union alongside Z. */
#define ZW_SWIZZLES \
  VecSwizzleReadWrite<T, 2> zw, ba; \
  VecSwizzleReadOnly<T, 2, 3, 2> wz, ab; \
  VecSwizzleReadOnly<T, 3, 2, 2, 3> zzw, bba; \
  VecSwizzleReadOnly<T, 3, 2, 3, 2> zwz, bab; \
  VecSwizzleReadOnly<T, 3, 2, 3, 3> zww, baa; \
  VecSwizzleReadOnly<T, 3, 3, 2, 2> wzz, abb; \
  VecSwizzleReadOnly<T, 3, 3, 2, 3> wzw, aba; \
  VecSwizzleReadOnly<T, 3, 3, 3, 2> wwz, aab; \
  VecSwizzleReadOnly<T, 4, 2, 2, 2, 3> zzzw, bbba; \
  VecSwizzleReadOnly<T, 4, 2, 2, 3, 2> zzwz, bbab; \
  VecSwizzleReadOnly<T, 4, 2, 2, 3, 3> zzww, bbaa; \
  VecSwizzleReadOnly<T, 4, 2, 3, 2, 2> zwzz, babb; \
  VecSwizzleReadOnly<T, 4, 2, 3, 2, 3> zwzw, baba; \
  VecSwizzleReadOnly<T, 4, 2, 3, 3, 2> zwwz, baab; \
  VecSwizzleReadOnly<T, 4, 2, 3, 3, 3> zwww, baaa; \
  VecSwizzleReadOnly<T, 4, 3, 2, 2, 2> wzzz, abbb; \
  VecSwizzleReadOnly<T, 4, 3, 2, 2, 3> wzzw, abba; \
  VecSwizzleReadOnly<T, 4, 3, 2, 3, 2> wzwz, abab; \
  VecSwizzleReadOnly<T, 4, 3, 2, 3, 3> wzww, abaa; \
  VecSwizzleReadOnly<T, 4, 3, 3, 2, 2> wwzz, aabb; \
  VecSwizzleReadOnly<T, 4, 3, 3, 2, 3> wwzw, aaba; \
  VecSwizzleReadOnly<T, 4, 3, 3, 3, 2> wwwz, aaab;

/* Swizzles containing XYZ. Must be declared in a union alongside X. */
#define XYZ_SWIZZLES \
  VecSwizzleReadWrite<T, 3> xyz, rgb; \
  VecSwizzleReadOnly<T, 3, 0, 2, 1> xzy, rbg; \
  VecSwizzleReadOnly<T, 3, 1, 0, 2> yxz, grb; \
  VecSwizzleReadOnly<T, 3, 1, 2, 0> yzx, gbr; \
  VecSwizzleReadOnly<T, 3, 2, 0, 1> zxy, brg; \
  VecSwizzleReadOnly<T, 3, 2, 1, 0> zyx, bgr; \
  VecSwizzleReadOnly<T, 4, 0, 0, 1, 2> xxyz, rrgb; \
  VecSwizzleReadOnly<T, 4, 0, 0, 2, 1> xxzy, rrbg; \
  VecSwizzleReadOnly<T, 4, 0, 1, 0, 2> xyxz, rgrb; \
  VecSwizzleReadOnly<T, 4, 0, 1, 1, 2> xyyz, rggb; \
  VecSwizzleReadOnly<T, 4, 0, 1, 2, 0> xyzx, rgbr; \
  VecSwizzleReadOnly<T, 4, 0, 1, 2, 1> xyzy, rgbg; \
  VecSwizzleReadOnly<T, 4, 0, 1, 2, 2> xyzz, rgbb; \
  VecSwizzleReadOnly<T, 4, 0, 2, 0, 1> xzxy, rbrg; \
  VecSwizzleReadOnly<T, 4, 0, 2, 1, 0> xzyx, rbgr; \
  VecSwizzleReadOnly<T, 4, 0, 2, 1, 1> xzyy, rbgg; \
  VecSwizzleReadOnly<T, 4, 0, 2, 1, 2> xzyz, rbgb; \
  VecSwizzleReadOnly<T, 4, 0, 2, 2, 1> xzzy, rbbg; \
  VecSwizzleReadOnly<T, 4, 1, 0, 0, 2> yxxz, grrb; \
  VecSwizzleReadOnly<T, 4, 1, 0, 1, 2> yxyz, grgb; \
  VecSwizzleReadOnly<T, 4, 1, 0, 2, 0> yxzx, grbr; \
  VecSwizzleReadOnly<T, 4, 1, 0, 2, 1> yxzy, grbg; \
  VecSwizzleReadOnly<T, 4, 1, 0, 2, 2> yxzz, grbb; \
  VecSwizzleReadOnly<T, 4, 1, 1, 0, 2> yyxz, ggrb; \
  VecSwizzleReadOnly<T, 4, 1, 1, 2, 0> yyzx, ggbr; \
  VecSwizzleReadOnly<T, 4, 1, 2, 0, 0> yzxx, gbrr; \
  VecSwizzleReadOnly<T, 4, 1, 2, 0, 1> yzxy, gbrg; \
  VecSwizzleReadOnly<T, 4, 1, 2, 0, 2> yzxz, gbrb; \
  VecSwizzleReadOnly<T, 4, 1, 2, 1, 0> yzyx, gbgr; \
  VecSwizzleReadOnly<T, 4, 1, 2, 2, 0> yzzx, gbbr; \
  VecSwizzleReadOnly<T, 4, 2, 0, 0, 1> zxxy, brrg; \
  VecSwizzleReadOnly<T, 4, 2, 0, 1, 0> zxyx, brgr; \
  VecSwizzleReadOnly<T, 4, 2, 0, 1, 1> zxyy, brgg; \
  VecSwizzleReadOnly<T, 4, 2, 0, 1, 2> zxyz, brgb; \
  VecSwizzleReadOnly<T, 4, 2, 0, 2, 1> zxzy, brbg; \
  VecSwizzleReadOnly<T, 4, 2, 1, 0, 0> zyxx, bgrr; \
  VecSwizzleReadOnly<T, 4, 2, 1, 0, 1> zyxy, bgrg; \
  VecSwizzleReadOnly<T, 4, 2, 1, 0, 2> zyxz, bgrb; \
  VecSwizzleReadOnly<T, 4, 2, 1, 1, 0> zyyx, bggr; \
  VecSwizzleReadOnly<T, 4, 2, 1, 2, 0> zyzx, bgbr; \
  VecSwizzleReadOnly<T, 4, 2, 2, 0, 1> zzxy, bbrg; \
  VecSwizzleReadOnly<T, 4, 2, 2, 1, 0> zzyx, bbgr;

/* Swizzles containing YZW. Must be declared in a union alongside Y. */
#define YZW_SWIZZLES \
  VecSwizzleReadWrite<T, 3> yzw, gba; \
  VecSwizzleReadOnly<T, 3, 1, 3, 2> ywz, gab; \
  VecSwizzleReadOnly<T, 3, 2, 1, 3> zyw, bga; \
  VecSwizzleReadOnly<T, 3, 2, 3, 1> zwy, bag; \
  VecSwizzleReadOnly<T, 3, 3, 1, 2> wyz, agb; \
  VecSwizzleReadOnly<T, 3, 3, 2, 1> wzy, abg; \
  VecSwizzleReadOnly<T, 4, 1, 1, 2, 3> yyzw, ggba; \
  VecSwizzleReadOnly<T, 4, 1, 1, 3, 2> yywz, ggab; \
  VecSwizzleReadOnly<T, 4, 1, 2, 1, 3> yzyw, gbga; \
  VecSwizzleReadOnly<T, 4, 1, 2, 2, 3> yzzw, gbba; \
  VecSwizzleReadOnly<T, 4, 1, 2, 3, 1> yzwy, gbag; \
  VecSwizzleReadOnly<T, 4, 1, 2, 3, 2> yzwz, gbab; \
  VecSwizzleReadOnly<T, 4, 1, 2, 3, 3> yzww, gbaa; \
  VecSwizzleReadOnly<T, 4, 1, 3, 1, 2> ywyz, gagb; \
  VecSwizzleReadOnly<T, 4, 1, 3, 2, 1> ywzy, gabg; \
  VecSwizzleReadOnly<T, 4, 1, 3, 2, 2> ywzz, gabb; \
  VecSwizzleReadOnly<T, 4, 1, 3, 2, 3> ywzw, gaba; \
  VecSwizzleReadOnly<T, 4, 1, 3, 3, 2> ywwz, gaab; \
  VecSwizzleReadOnly<T, 4, 2, 1, 1, 3> zyyw, bgga; \
  VecSwizzleReadOnly<T, 4, 2, 1, 2, 3> zyzw, bgba; \
  VecSwizzleReadOnly<T, 4, 2, 1, 3, 1> zywy, bgag; \
  VecSwizzleReadOnly<T, 4, 2, 1, 3, 2> zywz, bgab; \
  VecSwizzleReadOnly<T, 4, 2, 1, 3, 3> zyww, bgaa; \
  VecSwizzleReadOnly<T, 4, 2, 2, 1, 3> zzyw, bbga; \
  VecSwizzleReadOnly<T, 4, 2, 2, 3, 1> zzwy, bbag; \
  VecSwizzleReadOnly<T, 4, 2, 3, 1, 1> zwyy, bagg; \
  VecSwizzleReadOnly<T, 4, 2, 3, 1, 2> zwyz, bagb; \
  VecSwizzleReadOnly<T, 4, 2, 3, 1, 3> zwyw, baga; \
  VecSwizzleReadOnly<T, 4, 2, 3, 2, 1> zwzy, babg; \
  VecSwizzleReadOnly<T, 4, 2, 3, 3, 1> zwwy, baag; \
  VecSwizzleReadOnly<T, 4, 3, 1, 1, 2> wyyz, aggb; \
  VecSwizzleReadOnly<T, 4, 3, 1, 2, 1> wyzy, agbg; \
  VecSwizzleReadOnly<T, 4, 3, 1, 2, 2> wyzz, agbb; \
  VecSwizzleReadOnly<T, 4, 3, 1, 2, 3> wyzw, agba; \
  VecSwizzleReadOnly<T, 4, 3, 1, 3, 2> wywz, agab; \
  VecSwizzleReadOnly<T, 4, 3, 2, 1, 1> wzyy, abgg; \
  VecSwizzleReadOnly<T, 4, 3, 2, 1, 2> wzyz, abgb; \
  VecSwizzleReadOnly<T, 4, 3, 2, 1, 3> wzyw, abga; \
  VecSwizzleReadOnly<T, 4, 3, 2, 2, 1> wzzy, abbg; \
  VecSwizzleReadOnly<T, 4, 3, 2, 3, 1> wzwy, abag; \
  VecSwizzleReadOnly<T, 4, 3, 3, 1, 2> wwyz, aagb; \
  VecSwizzleReadOnly<T, 4, 3, 3, 2, 1> wwzy, aabg;

/* Swizzles containing XYZW. Must be declared in a union alongside X. */
#define XYZW_SWIZZLES \
  VecSwizzleReadWrite<T, 4> xyzw, rgba; \
  VecSwizzleReadOnly<T, 4, 0, 1, 3, 2> xywz, rgab; \
  VecSwizzleReadOnly<T, 4, 0, 2, 1, 3> xzyw, rbga; \
  VecSwizzleReadOnly<T, 4, 0, 2, 3, 1> xzwy, rbag; \
  VecSwizzleReadOnly<T, 4, 0, 3, 1, 2> xwyz, ragb; \
  VecSwizzleReadOnly<T, 4, 0, 3, 2, 1> xwzy, rabg; \
  VecSwizzleReadOnly<T, 4, 1, 0, 2, 3> yxzw, grba; \
  VecSwizzleReadOnly<T, 4, 1, 0, 3, 2> yxwz, grab; \
  VecSwizzleReadOnly<T, 4, 1, 2, 0, 3> yzxw, gbra; \
  VecSwizzleReadOnly<T, 4, 1, 2, 3, 0> yzwx, gbar; \
  VecSwizzleReadOnly<T, 4, 1, 3, 0, 2> ywxz, garb; \
  VecSwizzleReadOnly<T, 4, 1, 3, 2, 0> ywzx, gabr; \
  VecSwizzleReadOnly<T, 4, 2, 0, 1, 3> zxyw, brga; \
  VecSwizzleReadOnly<T, 4, 2, 0, 3, 1> zxwy, brag; \
  VecSwizzleReadOnly<T, 4, 2, 1, 0, 3> zyxw, bgra; \
  VecSwizzleReadOnly<T, 4, 2, 1, 3, 0> zywx, bgar; \
  VecSwizzleReadOnly<T, 4, 2, 3, 0, 1> zwxy, barg; \
  VecSwizzleReadOnly<T, 4, 2, 3, 1, 0> zwyx, bagr; \
  VecSwizzleReadOnly<T, 4, 3, 0, 1, 2> wxyz, argb; \
  VecSwizzleReadOnly<T, 4, 3, 0, 2, 1> wxzy, arbg; \
  VecSwizzleReadOnly<T, 4, 3, 1, 0, 2> wyxz, agrb; \
  VecSwizzleReadOnly<T, 4, 3, 1, 2, 0> wyzx, agbr; \
  VecSwizzleReadOnly<T, 4, 3, 2, 0, 1> wzxy, abrg; \
  VecSwizzleReadOnly<T, 4, 3, 2, 1, 0> wzyx, abgr;

}  // namespace blender
