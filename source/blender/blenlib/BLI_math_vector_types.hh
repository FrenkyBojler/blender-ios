/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include <array>
#include <ostream>
#include <type_traits>

#include "BLI_math_vector_unroll.hh"
#include "BLI_utildefines.h"

namespace blender {

/* clang-format off */
template<typename T>
using as_uint_type = std::conditional_t<sizeof(T) == sizeof(uint8_t), uint8_t,
                     std::conditional_t<sizeof(T) == sizeof(uint16_t), uint16_t,
                     std::conditional_t<sizeof(T) == sizeof(uint32_t), uint32_t,
                     std::conditional_t<sizeof(T) == sizeof(uint64_t), uint64_t, void>>>>;
/* clang-format on */

template<typename T, int Size> struct VecBase;

namespace detail {
template<typename T> struct use_swizzle {
  static constexpr bool value = false;
};

template<> struct use_swizzle<float> {
  static constexpr bool value = true;
};
template<> struct use_swizzle<uint> {
  static constexpr bool value = true;
};
template<> struct use_swizzle<int> {
  static constexpr bool value = true;
};
}  // namespace detail

/**
 * Swizzle class that supports arithmetic operations.
 * Will decay to a vector of the same size.
 *
 * IMPORTANT: Must be declared at the same memory location as the first component referenced in the
 * swizzle. We do this to allow the copy constructor to copy only the referenced component in the
 * case of something like `a.zzy = b.zzy`. This is because we do not want to override (or delete)
 * the copy constructor as it would make the vector types non-trivial.
 */
template<typename T, int Size, int x_, int y_, int z_ = y_, int w_ = z_> struct vec_ro_swizzle {
  using VecT = VecBase<T, Size>;
  static constexpr int max_comp = std::max(std::max(std::max(x_, y_), z_), w_);
  static constexpr int min_comp = std::min(std::min(std::min(x_, y_), z_), w_);
  static constexpr int effective_len = max_comp - min_comp + 1;

 private:
  T values_[effective_len];

 public:
  vec_ro_swizzle() = default;

  operator VecT() const
  {
    if constexpr (Size == 4) {
      return {values_[x_ - min_comp],
              values_[y_ - min_comp],
              values_[z_ - min_comp],
              values_[w_ - min_comp]};
    }
    else if constexpr (Size == 3) {
      return {values_[x_ - min_comp], values_[y_ - min_comp], values_[z_ - min_comp]};
    }
    else if constexpr (Size == 2) {
      return {values_[x_ - min_comp], values_[y_ - min_comp]};
    }
    return {};
  }

  VecT operator()() const
  {
    return VecT(*this);
  }
};

/**
 * Swizzle class that supports assignment.
 * The writes are indirected to the actual swizzled memory location.
 * Can only be used if all swizzled components points to different memory locations.
 *
 * All operators needs take their argument by copy in case the input is another swizzle referencing
 * the same memory locations.
 */
template<typename T, int Size> struct vec_rw_swizzle {
  using VecT = VecBase<T, Size>;

 private:
  std::array<T, Size> values_;

 public:
  operator VecT() const
  {
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

  VecT operator()() const
  {
    return VecT(*this);
  }

  vec_rw_swizzle &operator=(const VecT &other)
  {
    return (*this = *reinterpret_cast<const vec_rw_swizzle *>(&other));
  }

  template<int x_, int y_, int z_, int w_>
  vec_rw_swizzle &operator=(const vec_ro_swizzle<T, Size, x_, y_, z_, w_> &other)
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

  STD_OP vec_rw_swizzle &operator+=(const VecT &a) IMPL_BINARY(+);
  STD_OP vec_rw_swizzle &operator-=(const VecT &a) IMPL_BINARY(-);
  STD_OP vec_rw_swizzle &operator/=(const VecT &a) IMPL_BINARY(/);
  STD_OP vec_rw_swizzle &operator*=(const VecT &a) IMPL_BINARY(*);

  STD_OP vec_rw_swizzle &operator+=(const T &a) IMPL_BINARY(+);
  STD_OP vec_rw_swizzle &operator-=(const T &a) IMPL_BINARY(-);
  STD_OP vec_rw_swizzle &operator/=(const T &a) IMPL_BINARY(/);
  STD_OP vec_rw_swizzle &operator*=(const T &a) IMPL_BINARY(*);

#define INT_OP \
  template<typename U = T, \
           typename std::enable_if_t<std::is_integral_v<U>> * = nullptr, \
           typename std::enable_if_t<!std::is_same_v<bool, U>> * = nullptr>

  INT_OP VecT operator~() const IMPL_UNARY(~);

  INT_OP vec_rw_swizzle &operator%=(const VecT &a) IMPL_BINARY(%);
  INT_OP vec_rw_swizzle &operator&=(const VecT &a) IMPL_BINARY(&);
  INT_OP vec_rw_swizzle &operator|=(const VecT &a) IMPL_BINARY(|);
  INT_OP vec_rw_swizzle &operator^=(const VecT &a) IMPL_BINARY(^);

  INT_OP vec_rw_swizzle &operator%=(const T &a) IMPL_BINARY(%);
  INT_OP vec_rw_swizzle &operator&=(const T &a) IMPL_BINARY(&);
  INT_OP vec_rw_swizzle &operator|=(const T &a) IMPL_BINARY(|);
  INT_OP vec_rw_swizzle &operator^=(const T &a) IMPL_BINARY(^);

  INT_OP vec_rw_swizzle &operator<<=(const VecT &a) IMPL_BINARY(<<);
  INT_OP vec_rw_swizzle &operator>>=(const VecT &a) IMPL_BINARY(>>);
  INT_OP vec_rw_swizzle &operator<<=(const T &a) IMPL_BINARY(<<);
  INT_OP vec_rw_swizzle &operator>>=(const T &a) IMPL_BINARY(>>);

#undef INT_OP
#undef IMPL_BINARY
#undef IMPL_UNARY
};

template<typename T, int Size, bool is_trivial_type> struct vec_struct_base {
  std::array<T, Size> values;
};

template<typename T> struct vec_struct_base<T, 2, false> {
  T x, y;

  /* Masking. */
  const VecBase<T, 2> &xy() const
  {
    return *reinterpret_cast<const VecBase<T, 2> *>(&x);
  }
};

template<typename T> struct vec_struct_base<T, 3, false> {
  T x, y, z;

  /* Masking. */
  const VecBase<T, 2> &xy() const
  {
    return *reinterpret_cast<const VecBase<T, 2> *>(&x);
  }

  const VecBase<T, 2> &yz() const
  {
    return *reinterpret_cast<const VecBase<T, 2> *>(&y);
  }

  const VecBase<T, 3> &xyz() const
  {
    return *reinterpret_cast<const VecBase<T, 3> *>(&x);
  }
};

template<typename T> struct vec_struct_base<T, 4, false> {
  T x, y, z, w;

  /* Masking. */
  const VecBase<T, 2> &xy() const
  {
    return *reinterpret_cast<const VecBase<T, 2> *>(&x);
  }

  const VecBase<T, 2> &yz() const
  {
    return *reinterpret_cast<const VecBase<T, 2> *>(&y);
  }

  const VecBase<T, 3> &xyz() const
  {
    return *reinterpret_cast<const VecBase<T, 3> *>(&x);
  }

  const VecBase<T, 2> &zw() const
  {
    return *reinterpret_cast<const VecBase<T, 2> *>(&z);
  }

  const VecBase<T, 3> &yzw() const
  {
    return *reinterpret_cast<const VecBase<T, 3> *>(&y);
  }

  const VecBase<T, 4> &xyzw() const
  {
    return *reinterpret_cast<const VecBase<T, 4> *>(&x);
  }
};

template<typename T> struct vec_struct_base<T, 2, true> {
  union {
#ifndef NDEBUG
    /* Easier to read inside a debugger. */
    std::array<T, 2> debug_values_;
#endif
    struct {
      T x;
      union {
        T y;
        /* Component extension. */
        vec_ro_swizzle<T, 2, 1, 1> yy;
        vec_ro_swizzle<T, 3, 1, 1, 1> yyy;
        vec_ro_swizzle<T, 4, 1, 1, 1, 1> yyyy;
      };
    };
#ifndef NDEBUG
    /* Nesting to avoid readability issues inside a debugger. */
    union {
#endif
      vec_rw_swizzle<T, 2> xy;
      /* Component extension. */
      vec_ro_swizzle<T, 2, 0, 0> xx;
      vec_ro_swizzle<T, 3, 0, 0, 0> xxx;
      vec_ro_swizzle<T, 4, 0, 0, 0, 0> xxxx;
      /* Swizzles containing XY. */
      vec_ro_swizzle<T, 2, 1, 0> yx;
      vec_ro_swizzle<T, 3, 0, 0, 1> xxy;
      vec_ro_swizzle<T, 3, 0, 1, 0> xyx;
      vec_ro_swizzle<T, 3, 0, 1, 1> xyy;
      vec_ro_swizzle<T, 3, 1, 0, 0> yxx;
      vec_ro_swizzle<T, 3, 1, 0, 1> yxy;
      vec_ro_swizzle<T, 3, 1, 1, 0> yyx;
      vec_ro_swizzle<T, 4, 0, 0, 0, 1> xxxy;
      vec_ro_swizzle<T, 4, 0, 0, 1, 0> xxyx;
      vec_ro_swizzle<T, 4, 0, 0, 1, 1> xxyy;
      vec_ro_swizzle<T, 4, 0, 1, 0, 0> xyxx;
      vec_ro_swizzle<T, 4, 0, 1, 0, 1> xyxy;
      vec_ro_swizzle<T, 4, 0, 1, 1, 0> xyyx;
      vec_ro_swizzle<T, 4, 0, 1, 1, 1> xyyy;
      vec_ro_swizzle<T, 4, 1, 0, 0, 0> yxxx;
      vec_ro_swizzle<T, 4, 1, 0, 0, 1> yxxy;
      vec_ro_swizzle<T, 4, 1, 0, 1, 0> yxyx;
      vec_ro_swizzle<T, 4, 1, 0, 1, 1> yxyy;
      vec_ro_swizzle<T, 4, 1, 1, 0, 0> yyxx;
      vec_ro_swizzle<T, 4, 1, 1, 0, 1> yyxy;
      vec_ro_swizzle<T, 4, 1, 1, 1, 0> yyyx;
#ifndef NDEBUG
    };
#endif
  };
};

template<typename T> struct vec_struct_base<T, 3, true> {
  union {
#ifndef NDEBUG
    /* Easier to read inside a debugger. */
    std::array<T, 3> debug_values_;
#endif
    struct {
      T x;
      union {
        struct {
          T y;
          union {
            T z;
            /* Component extension. */
            vec_ro_swizzle<T, 2, 2, 2> zz;
            vec_ro_swizzle<T, 3, 2, 2, 2> zzz;
            vec_ro_swizzle<T, 4, 2, 2, 2, 2> zzzz;
          };
        };
        vec_rw_swizzle<T, 2> yz;
        /* Component extension. */
        vec_ro_swizzle<T, 2, 1, 1> yy;
        vec_ro_swizzle<T, 3, 1, 1, 1> yyy;
        vec_ro_swizzle<T, 4, 1, 1, 1, 1> yyyy;
        /* Swizzles containing YZ. */
        vec_ro_swizzle<T, 2, 2, 1> zy;
        vec_ro_swizzle<T, 3, 1, 1, 2> yyz;
        vec_ro_swizzle<T, 3, 1, 2, 1> yzy;
        vec_ro_swizzle<T, 3, 1, 2, 2> yzz;
        vec_ro_swizzle<T, 3, 2, 1, 1> zyy;
        vec_ro_swizzle<T, 3, 2, 1, 2> zyz;
        vec_ro_swizzle<T, 3, 2, 2, 1> zzy;
        vec_ro_swizzle<T, 4, 1, 1, 1, 2> yyyz;
        vec_ro_swizzle<T, 4, 1, 1, 2, 1> yyzy;
        vec_ro_swizzle<T, 4, 1, 1, 2, 2> yyzz;
        vec_ro_swizzle<T, 4, 1, 2, 1, 1> yzyy;
        vec_ro_swizzle<T, 4, 1, 2, 1, 2> yzyz;
        vec_ro_swizzle<T, 4, 1, 2, 2, 1> yzzy;
        vec_ro_swizzle<T, 4, 1, 2, 2, 2> yzzz;
        vec_ro_swizzle<T, 4, 2, 1, 1, 1> zyyy;
        vec_ro_swizzle<T, 4, 2, 1, 1, 2> zyyz;
        vec_ro_swizzle<T, 4, 2, 1, 2, 1> zyzy;
        vec_ro_swizzle<T, 4, 2, 1, 2, 2> zyzz;
        vec_ro_swizzle<T, 4, 2, 2, 1, 1> zzyy;
        vec_ro_swizzle<T, 4, 2, 2, 1, 2> zzyz;
        vec_ro_swizzle<T, 4, 2, 2, 2, 1> zzzy;
      };
    };
#ifndef NDEBUG
    /* Nesting to avoid readability issues inside a debugger. */
    union {
#endif
      vec_rw_swizzle<T, 2> xy;
      vec_rw_swizzle<T, 3> xyz;
      /* Component extension. */
      vec_ro_swizzle<T, 2, 0, 0> xx;
      vec_ro_swizzle<T, 3, 0, 0, 0> xxx;
      vec_ro_swizzle<T, 4, 0, 0, 0, 0> xxxx;
      /* Swizzles containing XY. */
      vec_ro_swizzle<T, 2, 1, 0> yx;
      vec_ro_swizzle<T, 3, 0, 0, 1> xxy;
      vec_ro_swizzle<T, 3, 0, 1, 0> xyx;
      vec_ro_swizzle<T, 3, 0, 1, 1> xyy;
      vec_ro_swizzle<T, 3, 1, 0, 0> yxx;
      vec_ro_swizzle<T, 3, 1, 0, 1> yxy;
      vec_ro_swizzle<T, 3, 1, 1, 0> yyx;
      vec_ro_swizzle<T, 4, 0, 0, 0, 1> xxxy;
      vec_ro_swizzle<T, 4, 0, 0, 1, 0> xxyx;
      vec_ro_swizzle<T, 4, 0, 0, 1, 1> xxyy;
      vec_ro_swizzle<T, 4, 0, 1, 0, 0> xyxx;
      vec_ro_swizzle<T, 4, 0, 1, 0, 1> xyxy;
      vec_ro_swizzle<T, 4, 0, 1, 1, 0> xyyx;
      vec_ro_swizzle<T, 4, 0, 1, 1, 1> xyyy;
      vec_ro_swizzle<T, 4, 1, 0, 0, 0> yxxx;
      vec_ro_swizzle<T, 4, 1, 0, 0, 1> yxxy;
      vec_ro_swizzle<T, 4, 1, 0, 1, 0> yxyx;
      vec_ro_swizzle<T, 4, 1, 0, 1, 1> yxyy;
      vec_ro_swizzle<T, 4, 1, 1, 0, 0> yyxx;
      vec_ro_swizzle<T, 4, 1, 1, 0, 1> yyxy;
      vec_ro_swizzle<T, 4, 1, 1, 1, 0> yyyx;
      /* Swizzles containing XYZ. */
      vec_ro_swizzle<T, 3, 0, 2, 1> xzy;
      vec_ro_swizzle<T, 3, 1, 0, 2> yxz;
      vec_ro_swizzle<T, 3, 1, 2, 0> yzx;
      vec_ro_swizzle<T, 3, 2, 0, 1> zxy;
      vec_ro_swizzle<T, 3, 2, 1, 0> zyx;
      vec_ro_swizzle<T, 4, 0, 0, 1, 2> xxyz;
      vec_ro_swizzle<T, 4, 0, 0, 2, 1> xxzy;
      vec_ro_swizzle<T, 4, 0, 1, 0, 2> xyxz;
      vec_ro_swizzle<T, 4, 0, 1, 1, 2> xyyz;
      vec_ro_swizzle<T, 4, 0, 1, 2, 0> xyzx;
      vec_ro_swizzle<T, 4, 0, 1, 2, 1> xyzy;
      vec_ro_swizzle<T, 4, 0, 1, 2, 2> xyzz;
      vec_ro_swizzle<T, 4, 0, 2, 0, 1> xzxy;
      vec_ro_swizzle<T, 4, 0, 2, 1, 0> xzyx;
      vec_ro_swizzle<T, 4, 0, 2, 1, 1> xzyy;
      vec_ro_swizzle<T, 4, 0, 2, 1, 2> xzyz;
      vec_ro_swizzle<T, 4, 0, 2, 2, 1> xzzy;
      vec_ro_swizzle<T, 4, 1, 0, 0, 2> yxxz;
      vec_ro_swizzle<T, 4, 1, 0, 1, 2> yxyz;
      vec_ro_swizzle<T, 4, 1, 0, 2, 0> yxzx;
      vec_ro_swizzle<T, 4, 1, 0, 2, 1> yxzy;
      vec_ro_swizzle<T, 4, 1, 0, 2, 2> yxzz;
      vec_ro_swizzle<T, 4, 1, 1, 0, 2> yyxz;
      vec_ro_swizzle<T, 4, 1, 1, 2, 0> yyzx;
      vec_ro_swizzle<T, 4, 1, 2, 0, 0> yzxx;
      vec_ro_swizzle<T, 4, 1, 2, 0, 1> yzxy;
      vec_ro_swizzle<T, 4, 1, 2, 0, 2> yzxz;
      vec_ro_swizzle<T, 4, 1, 2, 1, 0> yzyx;
      vec_ro_swizzle<T, 4, 1, 2, 2, 0> yzzx;
      vec_ro_swizzle<T, 4, 2, 0, 0, 1> zxxy;
      vec_ro_swizzle<T, 4, 2, 0, 1, 0> zxyx;
      vec_ro_swizzle<T, 4, 2, 0, 1, 1> zxyy;
      vec_ro_swizzle<T, 4, 2, 0, 1, 2> zxyz;
      vec_ro_swizzle<T, 4, 2, 0, 2, 1> zxzy;
      vec_ro_swizzle<T, 4, 2, 1, 0, 0> zyxx;
      vec_ro_swizzle<T, 4, 2, 1, 0, 1> zyxy;
      vec_ro_swizzle<T, 4, 2, 1, 0, 2> zyxz;
      vec_ro_swizzle<T, 4, 2, 1, 1, 0> zyyx;
      vec_ro_swizzle<T, 4, 2, 1, 2, 0> zyzx;
      vec_ro_swizzle<T, 4, 2, 2, 0, 1> zzxy;
      vec_ro_swizzle<T, 4, 2, 2, 1, 0> zzyx;
#ifndef NDEBUG
    };
#endif
  };
};

template<typename T> struct vec_struct_base<T, 4, true> {
  union {
#ifndef NDEBUG
    /* Useful for debugging. */
    std::array<T, 4> debug_values_;
#endif
    struct {
      T x;
      union {
        struct {
          T y;
          union {
            struct {
              T z;
              union {
                T w;
                /* Component extension. */
                vec_ro_swizzle<T, 2, 3, 3> ww;
                vec_ro_swizzle<T, 3, 3, 3, 3> www;
                vec_ro_swizzle<T, 4, 3, 3, 3, 3> wwww;
              };
            };
            vec_rw_swizzle<T, 2> zw;
            /* Component extension. */
            vec_ro_swizzle<T, 2, 2, 2> zz;
            vec_ro_swizzle<T, 3, 2, 2, 2> zzz;
            vec_ro_swizzle<T, 4, 2, 2, 2, 2> zzzz;
            /* Swizzles containing ZW. */
            vec_ro_swizzle<T, 2, 3, 2> wz;
            vec_ro_swizzle<T, 3, 2, 2, 3> zzw;
            vec_ro_swizzle<T, 3, 2, 3, 2> zwz;
            vec_ro_swizzle<T, 3, 2, 3, 3> zww;
            vec_ro_swizzle<T, 3, 3, 2, 2> wzz;
            vec_ro_swizzle<T, 3, 3, 2, 3> wzw;
            vec_ro_swizzle<T, 3, 3, 3, 2> wwz;
            vec_ro_swizzle<T, 4, 2, 2, 2, 3> zzzw;
            vec_ro_swizzle<T, 4, 2, 2, 3, 2> zzwz;
            vec_ro_swizzle<T, 4, 2, 2, 3, 3> zzww;
            vec_ro_swizzle<T, 4, 2, 3, 2, 2> zwzz;
            vec_ro_swizzle<T, 4, 2, 3, 2, 3> zwzw;
            vec_ro_swizzle<T, 4, 2, 3, 3, 2> zwwz;
            vec_ro_swizzle<T, 4, 2, 3, 3, 3> zwww;
            vec_ro_swizzle<T, 4, 3, 2, 2, 2> wzzz;
            vec_ro_swizzle<T, 4, 3, 2, 2, 3> wzzw;
            vec_ro_swizzle<T, 4, 3, 2, 3, 2> wzwz;
            vec_ro_swizzle<T, 4, 3, 2, 3, 3> wzww;
            vec_ro_swizzle<T, 4, 3, 3, 2, 2> wwzz;
            vec_ro_swizzle<T, 4, 3, 3, 2, 3> wwzw;
            vec_ro_swizzle<T, 4, 3, 3, 3, 2> wwwz;
          };
        };
        vec_rw_swizzle<T, 2> yz;
        vec_rw_swizzle<T, 3> yzw;
        /* Component extension. */
        vec_ro_swizzle<T, 2, 1, 1> yy;
        vec_ro_swizzle<T, 3, 1, 1, 1> yyy;
        vec_ro_swizzle<T, 4, 1, 1, 1, 1> yyyy;
        /* Swizzles containing YZ. */
        vec_ro_swizzle<T, 2, 2, 1> zy;
        vec_ro_swizzle<T, 3, 1, 1, 2> yyz;
        vec_ro_swizzle<T, 3, 1, 2, 1> yzy;
        vec_ro_swizzle<T, 3, 1, 2, 2> yzz;
        vec_ro_swizzle<T, 3, 2, 1, 1> zyy;
        vec_ro_swizzle<T, 3, 2, 1, 2> zyz;
        vec_ro_swizzle<T, 3, 2, 2, 1> zzy;
        vec_ro_swizzle<T, 4, 1, 1, 1, 2> yyyz;
        vec_ro_swizzle<T, 4, 1, 1, 2, 1> yyzy;
        vec_ro_swizzle<T, 4, 1, 1, 2, 2> yyzz;
        vec_ro_swizzle<T, 4, 1, 2, 1, 1> yzyy;
        vec_ro_swizzle<T, 4, 1, 2, 1, 2> yzyz;
        vec_ro_swizzle<T, 4, 1, 2, 2, 1> yzzy;
        vec_ro_swizzle<T, 4, 1, 2, 2, 2> yzzz;
        vec_ro_swizzle<T, 4, 2, 1, 1, 1> zyyy;
        vec_ro_swizzle<T, 4, 2, 1, 1, 2> zyyz;
        vec_ro_swizzle<T, 4, 2, 1, 2, 1> zyzy;
        vec_ro_swizzle<T, 4, 2, 1, 2, 2> zyzz;
        vec_ro_swizzle<T, 4, 2, 2, 1, 1> zzyy;
        vec_ro_swizzle<T, 4, 2, 2, 1, 2> zzyz;
        vec_ro_swizzle<T, 4, 2, 2, 2, 1> zzzy;
        /* Swizzles containing YZW. */
        vec_ro_swizzle<T, 3, 1, 3, 2> ywz;
        vec_ro_swizzle<T, 3, 2, 1, 3> zyw;
        vec_ro_swizzle<T, 3, 2, 3, 1> zwy;
        vec_ro_swizzle<T, 3, 3, 1, 2> wyz;
        vec_ro_swizzle<T, 3, 3, 2, 1> wzy;
        vec_ro_swizzle<T, 4, 1, 1, 2, 3> yyzw;
        vec_ro_swizzle<T, 4, 1, 1, 3, 2> yywz;
        vec_ro_swizzle<T, 4, 1, 2, 1, 3> yzyw;
        vec_ro_swizzle<T, 4, 1, 2, 2, 3> yzzw;
        vec_ro_swizzle<T, 4, 1, 2, 3, 1> yzwy;
        vec_ro_swizzle<T, 4, 1, 2, 3, 2> yzwz;
        vec_ro_swizzle<T, 4, 1, 2, 3, 3> yzww;
        vec_ro_swizzle<T, 4, 1, 3, 1, 2> ywyz;
        vec_ro_swizzle<T, 4, 1, 3, 2, 1> ywzy;
        vec_ro_swizzle<T, 4, 1, 3, 2, 2> ywzz;
        vec_ro_swizzle<T, 4, 1, 3, 2, 3> ywzw;
        vec_ro_swizzle<T, 4, 1, 3, 3, 2> ywwz;
        vec_ro_swizzle<T, 4, 2, 1, 1, 3> zyyw;
        vec_ro_swizzle<T, 4, 2, 1, 2, 3> zyzw;
        vec_ro_swizzle<T, 4, 2, 1, 3, 1> zywy;
        vec_ro_swizzle<T, 4, 2, 1, 3, 2> zywz;
        vec_ro_swizzle<T, 4, 2, 1, 3, 3> zyww;
        vec_ro_swizzle<T, 4, 2, 2, 1, 3> zzyw;
        vec_ro_swizzle<T, 4, 2, 2, 3, 1> zzwy;
        vec_ro_swizzle<T, 4, 2, 3, 1, 1> zwyy;
        vec_ro_swizzle<T, 4, 2, 3, 1, 2> zwyz;
        vec_ro_swizzle<T, 4, 2, 3, 1, 3> zwyw;
        vec_ro_swizzle<T, 4, 2, 3, 2, 1> zwzy;
        vec_ro_swizzle<T, 4, 2, 3, 3, 1> zwwy;
        vec_ro_swizzle<T, 4, 3, 1, 1, 2> wyyz;
        vec_ro_swizzle<T, 4, 3, 1, 2, 1> wyzy;
        vec_ro_swizzle<T, 4, 3, 1, 2, 2> wyzz;
        vec_ro_swizzle<T, 4, 3, 1, 2, 3> wyzw;
        vec_ro_swizzle<T, 4, 3, 1, 3, 2> wywz;
        vec_ro_swizzle<T, 4, 3, 2, 1, 1> wzyy;
        vec_ro_swizzle<T, 4, 3, 2, 1, 2> wzyz;
        vec_ro_swizzle<T, 4, 3, 2, 1, 3> wzyw;
        vec_ro_swizzle<T, 4, 3, 2, 2, 1> wzzy;
        vec_ro_swizzle<T, 4, 3, 2, 3, 1> wzwy;
        vec_ro_swizzle<T, 4, 3, 3, 1, 2> wwyz;
        vec_ro_swizzle<T, 4, 3, 3, 2, 1> wwzy;
      };
    };
#ifndef NDEBUG
    /* Nesting struct to avoid readability issues inside a debugger. */
    union {
#endif
      vec_rw_swizzle<T, 2> xy;
      vec_rw_swizzle<T, 3> xyz;
      vec_rw_swizzle<T, 4> xyzw;
      /* Component extension. */
      vec_ro_swizzle<T, 2, 0, 0> xx;
      vec_ro_swizzle<T, 3, 0, 0, 0> xxx;
      vec_ro_swizzle<T, 4, 0, 0, 0, 0> xxxx;
      /* Swizzles containing XY. */
      vec_ro_swizzle<T, 2, 1, 0> yx;
      vec_ro_swizzle<T, 3, 0, 0, 1> xxy;
      vec_ro_swizzle<T, 3, 0, 1, 0> xyx;
      vec_ro_swizzle<T, 3, 0, 1, 1> xyy;
      vec_ro_swizzle<T, 3, 1, 0, 0> yxx;
      vec_ro_swizzle<T, 3, 1, 0, 1> yxy;
      vec_ro_swizzle<T, 3, 1, 1, 0> yyx;
      vec_ro_swizzle<T, 4, 0, 0, 0, 1> xxxy;
      vec_ro_swizzle<T, 4, 0, 0, 1, 0> xxyx;
      vec_ro_swizzle<T, 4, 0, 0, 1, 1> xxyy;
      vec_ro_swizzle<T, 4, 0, 1, 0, 0> xyxx;
      vec_ro_swizzle<T, 4, 0, 1, 0, 1> xyxy;
      vec_ro_swizzle<T, 4, 0, 1, 1, 0> xyyx;
      vec_ro_swizzle<T, 4, 0, 1, 1, 1> xyyy;
      vec_ro_swizzle<T, 4, 1, 0, 0, 0> yxxx;
      vec_ro_swizzle<T, 4, 1, 0, 0, 1> yxxy;
      vec_ro_swizzle<T, 4, 1, 0, 1, 0> yxyx;
      vec_ro_swizzle<T, 4, 1, 0, 1, 1> yxyy;
      vec_ro_swizzle<T, 4, 1, 1, 0, 0> yyxx;
      vec_ro_swizzle<T, 4, 1, 1, 0, 1> yyxy;
      vec_ro_swizzle<T, 4, 1, 1, 1, 0> yyyx;
      /* Swizzles containing XYZ. */
      vec_ro_swizzle<T, 3, 0, 2, 1> xzy;
      vec_ro_swizzle<T, 3, 1, 0, 2> yxz;
      vec_ro_swizzle<T, 3, 1, 2, 0> yzx;
      vec_ro_swizzle<T, 3, 2, 0, 1> zxy;
      vec_ro_swizzle<T, 3, 2, 1, 0> zyx;
      vec_ro_swizzle<T, 4, 0, 0, 1, 2> xxyz;
      vec_ro_swizzle<T, 4, 0, 0, 2, 1> xxzy;
      vec_ro_swizzle<T, 4, 0, 1, 0, 2> xyxz;
      vec_ro_swizzle<T, 4, 0, 1, 1, 2> xyyz;
      vec_ro_swizzle<T, 4, 0, 1, 2, 0> xyzx;
      vec_ro_swizzle<T, 4, 0, 1, 2, 1> xyzy;
      vec_ro_swizzle<T, 4, 0, 1, 2, 2> xyzz;
      vec_ro_swizzle<T, 4, 0, 2, 0, 1> xzxy;
      vec_ro_swizzle<T, 4, 0, 2, 1, 0> xzyx;
      vec_ro_swizzle<T, 4, 0, 2, 1, 1> xzyy;
      vec_ro_swizzle<T, 4, 0, 2, 1, 2> xzyz;
      vec_ro_swizzle<T, 4, 0, 2, 2, 1> xzzy;
      vec_ro_swizzle<T, 4, 1, 0, 0, 2> yxxz;
      vec_ro_swizzle<T, 4, 1, 0, 1, 2> yxyz;
      vec_ro_swizzle<T, 4, 1, 0, 2, 0> yxzx;
      vec_ro_swizzle<T, 4, 1, 0, 2, 1> yxzy;
      vec_ro_swizzle<T, 4, 1, 0, 2, 2> yxzz;
      vec_ro_swizzle<T, 4, 1, 1, 0, 2> yyxz;
      vec_ro_swizzle<T, 4, 1, 1, 2, 0> yyzx;
      vec_ro_swizzle<T, 4, 1, 2, 0, 0> yzxx;
      vec_ro_swizzle<T, 4, 1, 2, 0, 1> yzxy;
      vec_ro_swizzle<T, 4, 1, 2, 0, 2> yzxz;
      vec_ro_swizzle<T, 4, 1, 2, 1, 0> yzyx;
      vec_ro_swizzle<T, 4, 1, 2, 2, 0> yzzx;
      vec_ro_swizzle<T, 4, 2, 0, 0, 1> zxxy;
      vec_ro_swizzle<T, 4, 2, 0, 1, 0> zxyx;
      vec_ro_swizzle<T, 4, 2, 0, 1, 1> zxyy;
      vec_ro_swizzle<T, 4, 2, 0, 1, 2> zxyz;
      vec_ro_swizzle<T, 4, 2, 0, 2, 1> zxzy;
      vec_ro_swizzle<T, 4, 2, 1, 0, 0> zyxx;
      vec_ro_swizzle<T, 4, 2, 1, 0, 1> zyxy;
      vec_ro_swizzle<T, 4, 2, 1, 0, 2> zyxz;
      vec_ro_swizzle<T, 4, 2, 1, 1, 0> zyyx;
      vec_ro_swizzle<T, 4, 2, 1, 2, 0> zyzx;
      vec_ro_swizzle<T, 4, 2, 2, 0, 1> zzxy;
      vec_ro_swizzle<T, 4, 2, 2, 1, 0> zzyx;
      /* Swizzles containing XYZW. */
      vec_ro_swizzle<T, 4, 0, 1, 3, 2> xywz;
      vec_ro_swizzle<T, 4, 0, 2, 1, 3> xzyw;
      vec_ro_swizzle<T, 4, 0, 2, 3, 1> xzwy;
      vec_ro_swizzle<T, 4, 0, 3, 1, 2> xwyz;
      vec_ro_swizzle<T, 4, 0, 3, 2, 1> xwzy;
      vec_ro_swizzle<T, 4, 1, 0, 2, 3> yxzw;
      vec_ro_swizzle<T, 4, 1, 0, 3, 2> yxwz;
      vec_ro_swizzle<T, 4, 1, 2, 0, 3> yzxw;
      vec_ro_swizzle<T, 4, 1, 2, 3, 0> yzwx;
      vec_ro_swizzle<T, 4, 1, 3, 0, 2> ywxz;
      vec_ro_swizzle<T, 4, 1, 3, 2, 0> ywzx;
      vec_ro_swizzle<T, 4, 2, 0, 1, 3> zxyw;
      vec_ro_swizzle<T, 4, 2, 0, 3, 1> zxwy;
      vec_ro_swizzle<T, 4, 2, 1, 0, 3> zyxw;
      vec_ro_swizzle<T, 4, 2, 1, 3, 0> zywx;
      vec_ro_swizzle<T, 4, 2, 3, 0, 1> zwxy;
      vec_ro_swizzle<T, 4, 2, 3, 1, 0> zwyx;
      vec_ro_swizzle<T, 4, 3, 0, 1, 2> wxyz;
      vec_ro_swizzle<T, 4, 3, 0, 2, 1> wxzy;
      vec_ro_swizzle<T, 4, 3, 1, 0, 2> wyxz;
      vec_ro_swizzle<T, 4, 3, 1, 2, 0> wyzx;
      vec_ro_swizzle<T, 4, 3, 2, 0, 1> wzxy;
      vec_ro_swizzle<T, 4, 3, 2, 1, 0> wzyx;
#ifndef NDEBUG
    };
#endif
  };
};

namespace math {

template<typename T> uint64_t vector_hash(const T &vec)
{
  BLI_STATIC_ASSERT(T::type_length <= 4, "Longer types need to implement vector_hash themself.");
  const typename T::uint_type &uvec = *reinterpret_cast<const typename T::uint_type *>(&vec);
  uint64_t result;
  result = uvec[0] * uint64_t(435109);
  if constexpr (T::type_length > 1) {
    result ^= uvec[1] * uint64_t(380867);
  }
  if constexpr (T::type_length > 2) {
    result ^= uvec[2] * uint64_t(1059217);
  }
  if constexpr (T::type_length > 3) {
    result ^= uvec[3] * uint64_t(2002613);
  }
  return result;
}

}  // namespace math

template<typename T, int Size>
struct VecBase : public vec_struct_base<T, Size, detail::use_swizzle<T>::value> {

  BLI_STATIC_ASSERT(alignof(T) <= sizeof(T),
                    "VecBase is not compatible with aligned type for now.");

/* Workaround issue with template BLI_ENABLE_IF((Size == 2)) not working. */
#define BLI_ENABLE_IF_VEC(_size, _test) int S = _size, BLI_ENABLE_IF((S _test))

  static constexpr int type_length = Size;

  using base_type = T;
  using uint_type = VecBase<as_uint_type<T>, Size>;

  VecBase() = default;

  template<BLI_ENABLE_IF_VEC(Size, > 1)> explicit VecBase(T value)
  {
    for (int i = 0; i < Size; i++) {
      (*this)[i] = value;
    }
  }

  template<typename U, BLI_ENABLE_IF((std::is_convertible_v<U, T>))>
  explicit VecBase(U value) : VecBase(T(value))
  {
  }

  template<BLI_ENABLE_IF_VEC(Size, == 1)> constexpr VecBase(T _x)
  {
    this->x = _x;
  }

  template<BLI_ENABLE_IF_VEC(Size, == 2)> constexpr VecBase(T _x, T _y)
  {
    this->x = _x;
    this->y = _y;
  }

  template<BLI_ENABLE_IF_VEC(Size, == 3)> constexpr VecBase(T _x, T _y, T _z)
  {
    this->x = _x;
    this->y = _y;
    this->z = _z;
  }

  template<BLI_ENABLE_IF_VEC(Size, == 4)> constexpr VecBase(T _x, T _y, T _z, T _w)
  {
    this->x = _x;
    this->y = _y;
    this->z = _z;
    this->w = _w;
  }

  /** Mixed scalar-vector constructors. */

  template<typename U, BLI_ENABLE_IF_VEC(Size, == 3)>
  constexpr VecBase(const VecBase<U, 2> &xy, T z) : VecBase(T(xy.x), T(xy.y), z)
  {
  }

  template<typename U, BLI_ENABLE_IF_VEC(Size, == 3)>
  constexpr VecBase(T x, const VecBase<U, 2> &yz) : VecBase(x, T(yz.x), T(yz.y))
  {
  }

  template<typename U, BLI_ENABLE_IF_VEC(Size, == 4)>
  VecBase(VecBase<U, 3> xyz, T w) : VecBase(T(xyz.x), T(xyz.y), T(xyz.z), T(w))
  {
  }

  template<typename U, BLI_ENABLE_IF_VEC(Size, == 4)>
  VecBase(T x, VecBase<U, 3> yzw) : VecBase(T(x), T(yzw.x), T(yzw.y), T(yzw.z))
  {
  }

  template<typename U, typename V, BLI_ENABLE_IF_VEC(Size, == 4)>
  VecBase(VecBase<U, 2> xy, VecBase<V, 2> zw) : VecBase(T(xy.x), T(xy.y), T(zw.x), T(zw.y))
  {
  }

  template<typename U, BLI_ENABLE_IF_VEC(Size, == 4)>
  VecBase(VecBase<U, 2> xy, T z, T w) : VecBase(T(xy.x), T(xy.y), T(z), T(w))
  {
  }

  template<typename U, BLI_ENABLE_IF_VEC(Size, == 4)>
  VecBase(T x, VecBase<U, 2> yz, T w) : VecBase(T(x), T(yz.x), T(yz.y), T(w))
  {
  }

  template<typename U, BLI_ENABLE_IF_VEC(Size, == 4)>
  VecBase(T x, T y, VecBase<U, 2> zw) : VecBase(T(x), T(y), T(zw.x), T(zw.y))
  {
  }

  /**
   * Prevent up-cast of dimensions (creating a bigger vector initialized with data
   * from a smaller one) by deleting all copy constructors accepting smaller vectors
   * as source.
   */
  template<typename U, int OtherSize, BLI_ENABLE_IF(OtherSize < Size)>
  VecBase(const VecBase<U, OtherSize> &other) = delete;

  /** Masking. */

  template<typename U, int OtherSize, BLI_ENABLE_IF(OtherSize > Size)>
  explicit VecBase(const VecBase<U, OtherSize> &other)
  {
    for (int i = 0; i < Size; i++) {
      (*this)[i] = T(other[i]);
    }
  }

#undef BLI_ENABLE_IF_VEC

  /** Conversion from pointers (from C-style vectors). */

  /* False positive warning with GCC: it sees array access like [3] but
   * input is only a 3-element array. But it fails to realize that the
   * [3] access is within "if constexpr (Size == 4)" check already. */
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
#endif

  VecBase(const T *ptr)
  {
    BLI_UNROLL_MATH_VEC_OP_INIT_INDEX(ptr);
  }

  template<typename U, BLI_ENABLE_IF((std::is_convertible_v<U, T>))> explicit VecBase(const U *ptr)
  {
    BLI_UNROLL_MATH_VEC_OP_INIT_INDEX(ptr);
  }

  VecBase(const T (*ptr)[Size]) : VecBase(static_cast<const T *>(ptr[0])) {}

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

  /** Conversion from other vector types. */

  template<typename U> explicit VecBase(const VecBase<U, Size> &vec)
  {
    BLI_UNROLL_MATH_VEC_OP_INIT_VECTOR(vec);
  }

  /** C-style pointer dereference. */

  operator const T *() const
  {
    return reinterpret_cast<const T *>(this);
  }

  operator T *()
  {
    return reinterpret_cast<T *>(this);
  }

  /** Array access. */

  const T &operator[](int index) const
  {
    BLI_assert(index >= 0);
    BLI_assert(index < Size);
    return reinterpret_cast<const T *>(this)[index];
  }

  T &operator[](int index)
  {
    BLI_assert(index >= 0);
    BLI_assert(index < Size);
    return reinterpret_cast<T *>(this)[index];
  }

  /** Internal Operators Macro. */

#define BLI_INT_OP(_T) template<typename U = _T, BLI_ENABLE_IF((std::is_integral_v<U>))>

  /** Arithmetic operators. */

  friend VecBase operator+(const VecBase &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(+, a, b);
  }

  friend VecBase operator+(const VecBase &a, const T &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(+, a, b);
  }

  friend VecBase operator+(const T &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_SCALAR_VEC(+, a, b);
  }

  VecBase &operator+=(const VecBase &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_VEC(+=, b);
  }

  VecBase &operator+=(const T &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_SCALAR(+=, b);
  }

  friend VecBase operator-(const VecBase &a)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC(-, a);
  }

  friend VecBase operator-(const VecBase &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(-, a, b);
  }

  friend VecBase operator-(const VecBase &a, const T &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(-, a, b);
  }

  friend VecBase operator-(const T &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_SCALAR_VEC(-, a, b);
  }

  VecBase &operator-=(const VecBase &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_VEC(-=, b);
  }

  VecBase &operator-=(const T &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_SCALAR(-=, b);
  }

  friend VecBase operator*(const VecBase &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(*, a, b);
  }

  template<typename FactorT> friend VecBase operator*(const VecBase &a, FactorT b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(*, a, b);
  }

  friend VecBase operator*(T a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_SCALAR_VEC(*, a, b);
  }

  VecBase &operator*=(T b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_SCALAR(*=, b);
  }

  VecBase &operator*=(const VecBase &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_VEC(*=, b);
  }

  friend VecBase operator/(const VecBase &a, const VecBase &b)
  {
    for (int i = 0; i < Size; i++) {
      BLI_assert(b[i] != T(0));
    }
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(/, a, b);
  }

  friend VecBase operator/(const VecBase &a, T b)
  {
    BLI_assert(b != T(0));
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(/, a, b);
  }

  friend VecBase operator/(T a, const VecBase &b)
  {
    for (int i = 0; i < Size; i++) {
      BLI_assert(b[i] != T(0));
    }
    BLI_UNROLL_MATH_VEC_OP_SCALAR_VEC(/, a, b);
  }

  VecBase &operator/=(T b) &
  {
    BLI_assert(b != T(0));
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_SCALAR(/=, b);
  }

  VecBase &operator/=(const VecBase &b) &
  {
    for (int i = 0; i < Size; i++) {
      BLI_assert(b[i] != T(0));
    }
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_VEC(/=, b);
  }

  /** Binary operators. */

  BLI_INT_OP(T) friend VecBase operator&(const VecBase &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(&, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator&(const VecBase &a, T b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(&, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator&(T a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_SCALAR_VEC(&, a, b);
  }

  BLI_INT_OP(T) VecBase &operator&=(T b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_SCALAR(&=, b);
  }

  BLI_INT_OP(T) VecBase &operator&=(const VecBase &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_VEC(&=, b);
  }

  BLI_INT_OP(T) friend VecBase operator|(const VecBase &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(|, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator|(const VecBase &a, T b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(|, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator|(T a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_SCALAR_VEC(|, a, b);
  }

  BLI_INT_OP(T) VecBase &operator|=(T b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_SCALAR(|=, b);
  }

  BLI_INT_OP(T) VecBase &operator|=(const VecBase &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_VEC(|=, b);
  }

  BLI_INT_OP(T) friend VecBase operator^(const VecBase &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(^, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator^(const VecBase &a, T b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(^, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator^(T a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_SCALAR_VEC(^, a, b);
  }

  BLI_INT_OP(T) VecBase &operator^=(T b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_SCALAR(^=, b);
  }

  BLI_INT_OP(T) VecBase &operator^=(const VecBase &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_VEC(^=, b);
  }

  BLI_INT_OP(T) friend VecBase operator~(const VecBase &a)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC(~, a);
  }

  /** Bit-shift operators. */

  BLI_INT_OP(T) friend VecBase operator<<(const VecBase &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(<<, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator<<(const VecBase &a, T b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(<<, a, b);
  }

  BLI_INT_OP(T) VecBase &operator<<=(T b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_SCALAR(<<=, b);
  }

  BLI_INT_OP(T) VecBase &operator<<=(const VecBase &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_VEC(<<=, b);
  }

  BLI_INT_OP(T) friend VecBase operator>>(const VecBase &a, const VecBase &b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(>>, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator>>(const VecBase &a, T b)
  {
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(>>, a, b);
  }

  BLI_INT_OP(T) VecBase &operator>>=(T b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_SCALAR(>>=, b);
  }

  BLI_INT_OP(T) VecBase &operator>>=(const VecBase &b) &
  {
    BLI_UNROLL_MATH_VEC_OP_ASSIGN_VEC(>>=, b);
  }

  /** Modulo operators. */

  BLI_INT_OP(T) friend VecBase operator%(const VecBase &a, const VecBase &b)
  {
    for (int i = 0; i < Size; i++) {
      BLI_assert(b[i] != T(0));
    }
    BLI_UNROLL_MATH_VEC_OP_VEC_VEC(%, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator%(const VecBase &a, T b)
  {
    BLI_assert(b != 0);
    BLI_UNROLL_MATH_VEC_OP_VEC_SCALAR(%, a, b);
  }

  BLI_INT_OP(T) friend VecBase operator%(T a, const VecBase &b)
  {
    for (int i = 0; i < Size; i++) {
      BLI_assert(b[i] != T(0));
    }
    BLI_UNROLL_MATH_VEC_OP_SCALAR_VEC(%, a, b);
  }

#undef BLI_INT_OP

  /** Compare. */

  friend bool operator==(const VecBase &a, const VecBase &b)
  {
    for (int i = 0; i < Size; i++) {
      if (a[i] != b[i]) {
        return false;
      }
    }
    return true;
  }

  friend bool operator!=(const VecBase &a, const VecBase &b)
  {
    return !(a == b);
  }

  /** Misc. */

  uint64_t hash() const
  {
    return math::vector_hash(*this);
  }

  friend std::ostream &operator<<(std::ostream &stream, const VecBase &v)
  {
    stream << "(";
    for (int i = 0; i < Size; i++) {
      stream << v[i];
      if (i != Size - 1) {
        stream << ", ";
      }
    }
    stream << ")";
    return stream;
  }
};

namespace math {

template<typename T> struct AssertUnitEpsilon {
  /** \note Copy of BLI_ASSERT_UNIT_EPSILON_DB to avoid dragging the entire header. */
  static constexpr T value = T(0.0002);
};

}  // namespace math

using char2 = blender::VecBase<int8_t, 2>;
using char3 = blender::VecBase<int8_t, 3>;
using char4 = blender::VecBase<int8_t, 4>;

using uchar2 = blender::VecBase<uint8_t, 2>;
using uchar3 = blender::VecBase<uint8_t, 3>;
using uchar4 = blender::VecBase<uint8_t, 4>;

using int2 = VecBase<int32_t, 2>;
using int3 = VecBase<int32_t, 3>;
using int4 = VecBase<int32_t, 4>;

using uint2 = VecBase<uint32_t, 2>;
using uint3 = VecBase<uint32_t, 3>;
using uint4 = VecBase<uint32_t, 4>;

using short2 = blender::VecBase<int16_t, 2>;
using short3 = blender::VecBase<int16_t, 3>;
using short4 = blender::VecBase<int16_t, 4>;

using ushort2 = VecBase<uint16_t, 2>;
using ushort3 = blender::VecBase<uint16_t, 3>;
using ushort4 = blender::VecBase<uint16_t, 4>;

using float1 = VecBase<float, 1>;
using float2 = VecBase<float, 2>;
using float3 = VecBase<float, 3>;
using float4 = VecBase<float, 4>;

using double2 = VecBase<double, 2>;
using double3 = VecBase<double, 3>;
using double4 = VecBase<double, 4>;

BLI_STATIC_ASSERT(std::is_trivially_constructible_v<float3>, "");
BLI_STATIC_ASSERT(std::is_trivially_copy_constructible_v<float3>, "");
BLI_STATIC_ASSERT(std::is_trivially_move_constructible_v<float3>, "");
BLI_STATIC_ASSERT(std::is_trivial_v<float3>, "");
BLI_STATIC_ASSERT(sizeof(float3) == 3 * sizeof(float), "");
// BLI_STATIC_ASSERT(sizeof(float3().x) == 1 * sizeof(float), "");
// BLI_STATIC_ASSERT(sizeof(float3().xx) == 1 * sizeof(float), "");
// BLI_STATIC_ASSERT(sizeof(float3().xxx) == 1 * sizeof(float), "");
// BLI_STATIC_ASSERT(sizeof(float3().xxxx) == 1 * sizeof(float), "");
// BLI_STATIC_ASSERT(sizeof(float3().xy) == 2 * sizeof(float), "");
// BLI_STATIC_ASSERT(sizeof(float3().yzz) == 2 * sizeof(float), "");
// BLI_STATIC_ASSERT(sizeof(float4().wyz) == 3 * sizeof(float), "");

}  // namespace blender
