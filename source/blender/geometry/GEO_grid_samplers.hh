/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_index_range.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include "BKE_volume_grid_type_traits.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/Interpolation.h>
#endif

namespace blender::geometry {

#ifdef WITH_OPENVDB

namespace grid_sampling {

template<int N, class AccessorT>
bool probe_values(const AccessorT &accessor,
                  const openvdb::CoordBBox &index_box,
                  typename AccessorT::ValueType (&data)[N][N][N])
{
  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  bool active = false;
  for (int dx : IndexRange(N)) {
    for (int dy : IndexRange(N)) {
      for (int dz : IndexRange(N)) {
        if (accessor.probeValue(index_box.getStart() + openvdb::Coord(dx, dy, dz),
                                data[dx][dy][dz]))
        {
          active = true;
        }
      }
    }
  }
  return active;
}

template<int N, class AccessorT>
void get_values(const AccessorT &accessor,
                const openvdb::CoordBBox &index_box,
                typename AccessorT::ValueType (&data)[N][N][N])
{
  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  for (int dx : IndexRange(N)) {
    for (int dy : IndexRange(N)) {
      for (int dz : IndexRange(N)) {
        data[dx][dy][dz] = accessor.getValue(index_box.getStart() + openvdb::Coord(dx, dy, dz));
      }
    }
  }
}

/* Interpolate values in 3D using a function that combines a 1-dimensional array. */
template<typename ValueT, int N, typename KernelFn>
void interpolate_value_3d(ValueT (&data)[N][N][N],
                          const openvdb::Vec3R &uvw,
                          KernelFn kernel_fn,
                          ValueT &result)
{
  ValueT vx[N];
  for (int dx : IndexRange(N)) {
    ValueT vy[N];
    for (int dy : IndexRange(N)) {
      const ValueT *vz = &data[dx][dy][0];
      vy[dy] = kernel_fn(vz, uvw.z());
    }
    vx[dx] = kernel_fn(vy, uvw.y());
  }
  result = kernel_fn(vx, uvw.x());
}

template<typename Kernel, class AccessorT>
bool sample_tree(const AccessorT &accessor,
                 const openvdb::Vec3R &coord,
                 typename AccessorT::ValueType &result)
{
  using ValueT = typename AccessorT::ValueType;

  const openvdb::Vec3R sample_offset = openvdb::Vec3R(Kernel::sample_offset);
  const openvdb::Coord index = openvdb::Coord(
      openvdb::tools::local_util::floorVec3(coord + sample_offset));
  const openvdb::CoordBBox index_box = openvdb::CoordBBox(
      index - openvdb::Coord(Kernel::samples_left - 1),
      index + openvdb::Coord(Kernel::samples_right));
  const openvdb::Vec3R uvw = coord - index;

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  constexpr int N = Kernel::samples_left + Kernel::samples_right;
  ValueT data[N][N][N];
  bool active = probe_values(accessor, index_box, data);
  interpolate_value_3d(data, uvw, Kernel::template sample_value<ValueT>, result);

  return active;
}

template<typename Kernel, class AccessorT>
typename AccessorT::ValueType sample_tree(const AccessorT &accessor, const openvdb::Vec3R &coord)
{
  using ValueT = typename AccessorT::ValueType;

  const openvdb::Vec3R sample_offset = openvdb::Vec3R(Kernel::sample_offset);
  const openvdb::Coord index = openvdb::Coord(
      openvdb::tools::local_util::floorVec3(coord + sample_offset));
  const openvdb::CoordBBox index_box = openvdb::CoordBBox(
      index - openvdb::Coord(Kernel::samples_left - 1),
      index + openvdb::Coord(Kernel::samples_right));
  const openvdb::Vec3R uvw = coord - index;

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  constexpr int N = Kernel::samples_left + Kernel::samples_right;
  ValueT data[N][N][N];
  get_values(accessor, index_box, data);

  ValueT result;
  interpolate_value_3d(data, uvw, Kernel::template sample_value<ValueT>, result);
  return result;
}

/**
 * Nearest-point kernel function.
 *
 * To avoid reading an unnecessary value (one value always has zero weight), the sampling interval
 * is shifted by 0.5. This is equivalent to rounding the sampling position to the nearest point.
 */
struct NearestPointKernel {
  static constexpr int samples_left = 1;
  static constexpr int samples_right = 0;
  /* Shift the sampling interval (round the coordinate), to use the value the closest point. */
  static constexpr float sample_offset = 0.5f;

  static float weight(float x)
  {
    if (x < 0.0f) {
      return 0.0f;
    }
    if (x < 1.0f) {
      return 1.0f;
    }
    return 0.0f;
  }

  static float derivative(float /*x*/)
  {
    return 0.0f;
  }

  template<class ValueT> static ValueT sample_value(const ValueT *values, float /*weight*/)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    return values[0];
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT> static ValueT sample_gradient(const ValueT * /*values*/, float /*weight*/)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    return ValueT(0.0);
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }
};

/**
 * Linear kernel function.
 */
struct LinearKernel {
  static constexpr int samples_left = 1;
  static constexpr int samples_right = 1;
  static constexpr float sample_offset = 0.0f;

  static float weight(float x)
  {
    if (x < -1.0f) {
      return 0.0f;
    }
    if (x < 0.0f) {
      return x + 1.0f;
    }
    if (x < 1.0f) {
      return -x + 1.0f;
    }
    return 0.0f;
  }

  static float derivative(float x)
  {
    if (x < -1.0f) {
      return 0.0f;
    }
    if (x < 0.0f) {
      return 1.0f;
    }
    if (x < 1.0f) {
      return -1.0f;
    }
    return 0.0f;
  }

  template<class ValueT> static ValueT sample_value(const ValueT *values, float weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    const ValueT lin = static_cast<ValueT>(values[1] - values[0]);
    const ValueT con = static_cast<ValueT>(values[0]);
    return static_cast<ValueT>(weight * lin) + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT> static ValueT sample_gradient(const ValueT *values, float /*weight*/)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    const ValueT con = static_cast<ValueT>(values[1] - values[0]);
    return con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }
};

/**
 * Quadratic B-spline kernel function as described in
 * Steffen et al., "Analysis and reduction of quadrature errors in the material point method (MPM)"
 *
 * The kernel is a piece-wise quadratic spline:
 * f(x) = 0                          for         x < -3/2
 * f(x) = 1/2*x^2 + 3/2*x + 9/8      for 1/2 <= x < 3/2
 * f(x) = -x^2 + 3/4                 for -1/2 <= x < 1/2
 * f(x) = 1/2*x^2 - 3/2*x + 9/8      for 1/2 <= x < 3/2
 * f(x) = 0                          for 3/2 <= x
 *
 * The derivative is a piece-wise linear function:
 * f(x) = 0                          for         x < -3/2
 * f(x) = x + 3/2                    for -3/2 <= x < -1/2
 * f(x) = -2*x                       for    0 <= x < 1/2
 * f(x) = x - 3/2                    for  1/2 <= x < 3/2
 * f(x) = 0                          for  3/2 <= x
 *
 * This kernel has a range of 1.5 voxels. For sampling in the index space of i <= x <= i+1
 * the contribution of points [i-1, i, i+1, i+2] must be considered.
 * Shifting the kernel function to these voxel locations yields these contributions:
 * v(x) = v[i-1]*f(x+1) +   v[i]*f(x) + v[i+1]*f(x-1) + v[i+2]*f(x-2)
 *      =      A*f(x+1) +      B*f(x) +      C*f(x-1) +      D*f(x-2)
 *
 * This results in the following expressions for sampling in one dimension:
 * For 0 <= x < 1/2:
 *   v(x) =   x^2*( 1/2*A     - B + 1/2*C)
 *          +   x*(-1/2*A         + 1/2*C)
 *          +     ( 1/8*A + 6/8*B + 1/8*C)
 * For 1/2 <= x < 1:
 *   v(x) =   x^2*( 1/2*B     - C + 1/2*D)
 *          +   x*(-3/2*B   + 2*C - 1/2*D)
 *          +     ( 9/8*B - 2/8*C + 1/8*D)
 *
 * and for the derivative:
 * For 0 <= x < 1/2:
 *   dv(x) =    x*(     A   - 2*B     + C)
 *           +    (-1/2*A         + 1/2*C)
 * For 1/2 <= x < 1:
 *   dv(x) =    x*(     B   - 2*C     + D)
 *           +    (-3/2*B   + 2*C - 1/2*D)
 */
struct QuadraticBSplineKernel {
  static constexpr int samples_left = 2;
  static constexpr int samples_right = 2;
  static constexpr float sample_offset = 0.0f;

  static float weight(float x)
  {
    if (x < -1.5f) {
      return 0.0f;
    }
    if (x < -0.5f) {
      return (0.5f * x + 1.5f) * x + 1.125f;
    }
    if (x < 0.5f) {
      return -x * x + 0.75f;
    }
    if (x < 1.5f) {
      return (0.5f * x - 1.5f) * x + 1.125f;
    }
    return 0.0f;
  }

  static float derivative(float x)
  {
    if (x < -1.5f) {
      return 0.0f;
    }
    if (x < -0.5f) {
      return x + 1.5f;
    }
    if (x < 0.5f) {
      return -2.0f * x;
    }
    if (x < 1.5f) {
      return x - 1.5f;
    }
    return 0.0f;
  }

  template<class ValueT> static ValueT sample_value(const ValueT *values, float weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    if (weight < 0.5) {
      const ValueT sqr = static_cast<ValueT>(0.5 * (values[0] + values[2]) - values[1]);
      const ValueT lin = static_cast<ValueT>(0.5 * (values[2] - values[0]));
      const ValueT con = static_cast<ValueT>(0.125 * (values[0] + values[2]) + 0.75 * values[1]);
      return weight * (weight * sqr + lin) + con;
    }

    const ValueT sqr = static_cast<ValueT>(0.5 * (values[1] + values[3]) - values[2]);
    const ValueT lin = static_cast<ValueT>(-1.5 * values[1] + 2 * values[2] - 0.5 * values[3]);
    const ValueT con = static_cast<ValueT>(1.125 * values[1] - 0.25 * values[2] +
                                           0.125 * values[3]);
    return weight * (weight * sqr + lin) + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT> static ValueT sample_gradient(const ValueT *values, float weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    if (weight < 0.5) {
      const ValueT lin = static_cast<ValueT>(values[0] - 2.0 * values[1] + values[2]);
      const ValueT con = static_cast<ValueT>(0.5 * (values[2] - values[0]));
      return weight * lin + con;
    }

    const ValueT lin = static_cast<ValueT>(values[1] - 2.0 * values[2] + values[3]);
    const ValueT con = static_cast<ValueT>(-1.5 * values[1] + 2.0 * values[2] - 0.5 * values[3]);
    return weight * lin + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }
};

/**
 * Cubic B-spline kernel function as described in
 * Steffen et al., "Analysis and reduction of quadrature errors in the material point method (MPM)"
 *
 * The kernel is a piece-wise cubic spline:
 * f(x) = 0                                 for       x  < -2
 * f(x) = 1/6*x^3 + x^2 + 2*x + 4/3         for  -2 <= x < -1
 * f(x) = -1/2*x^3 - x^2 + 2/3              for  -1 <= x < 0
 * f(x) = 1/2*x^3 - x^2 + 2/3               for   0 <= x < 1
 * f(x) = -1/6*x^3 + x^2 - 2*x + 4/3        for   1 <= x < 2
 * f(x) = 0                                 for   2 <= x
 *
 * The derivative is a piece-wise quadratic spline:
 * f(x) = 0                                 for        x < -2
 * f(x) = 1/2*x^2 + 2*x + 2                 for  -2 <= x < -1
 * f(x) = -3/2*x^2 - 2*x                    for  -1 <= x < 1
 * f(x) = 3/2*x^2 - 2*x                     for   0 <= x < 1
 * f(x) = -1/2*x^2 + 2*x - 2                for   1 <= x < 2
 * f(x) = 0                                 for   2 <= x
 *
 * This kernel has a range of 2 voxels. For sampling in the index space of i <= x <= i+1
 * the contribution of points [i-1, i, i+1, i+2] must be considered.
 * Shifting the kernel function to these voxel locations yields these contributions:
 * v(x) = v[i-1]*f(x+1) +   v[i]*f(x) + v[i+1]*f(x-1) + v[i+2]*f(x-2)
 *      =      A*f(x+1) +      B*f(x) +      C*f(x-1) +      D*f(x-2)
 *
 * This results in the following expression for sampling in one dimension:
 * v(x) =   x^3*(-1/6*A + 1/2*B - 1/2*C + 1/6*D)
 *        + x^2*( 1/2*A     - B + 1/2*C)
 *        + x  *(-1/2*A         + 1/2*C)
 *        +     ( 1/6*A + 2/3*B + 1/6*C)
 *
 * and for the derivative:
 * dv(x) =  x^2*(-1/2*A + 3/2*B - 3/2*C + 1/2*D)
 *        + x  *(     A   - 2*B     + C)
 *        +     (-1/2*A         + 1/2*C)
 */
struct CubicBSplineKernel {
  static constexpr int samples_left = 2;
  static constexpr int samples_right = 2;
  static constexpr float sample_offset = 0.0f;

  static float weight(float x)
  {
    if (x < -2.0f) {
      return 0.0f;
    }
    if (x < -1.0f) {
      return ((x / 6.0f + 1.0f) * x + 2.0f) * x + 4.0f / 3.0f;
    }
    if (x < 0.0f) {
      return (-x * 0.5f - 1.0f) * x * x + 2.0f / 3.0f;
    }
    if (x < 1.0f) {
      return (x * 0.5f - 1.0f) * x * x + 2.0f / 3.0f;
    }
    if (x < 2.0f) {
      return ((-x / 6.0f + 1.0f) * x - 2.0f) * x + 4.0f / 3.0f;
    }
    return 0.0f;
  }

  static float derivative(float x)
  {
    if (x < -2.0f) {
      return 0.0f;
    }
    if (x < -1.0f) {
      return (0.5 * x + 2.0f) * x + 2.0f;
    }
    if (x < 0.0f) {
      return (-1.5f * x - 2.0f) * x;
    }
    if (x < 1.0f) {
      return (1.5f * x - 2.0f) * x;
    }
    if (x < 2.0f) {
      return (-0.5 * x + 2.0f) * x - 2.0f;
    }
    return 0.0f;
  }

  template<class ValueT> static ValueT sample_value(const ValueT *values, float weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    constexpr double inv6 = 1.0 / 6.0;
    const ValueT cub = static_cast<ValueT>(inv6 * (values[3] - values[0]) +
                                           0.5 * (values[1] - values[2]));
    const ValueT sqr = static_cast<ValueT>(0.5 * (values[0] + values[2]) - values[1]);
    const ValueT lin = static_cast<ValueT>(0.5 * (values[2] - values[0]));
    const ValueT con = static_cast<ValueT>(inv6 * (values[0] + 4.0 * values[1] + values[2]));
    return weight * (weight * (weight * cub + sqr) + lin) + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT> static ValueT sample_gradient(const ValueT *values, float weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    const ValueT sqr = static_cast<ValueT>(0.5 * (values[3] - values[0]) +
                                           1.5 * (values[1] - values[2]));
    const ValueT lin = static_cast<ValueT>(values[0] - 2.0 * values[1] + values[2]);
    const ValueT con = static_cast<ValueT>(0.5 * (values[2] - values[0]));
    return weight * (weight * sqr + lin) + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }
};

/**
 * Grid value sampler using a kernel type.
 */
template<typename KernelT> struct SamplerWithKernel {
  template<class AccessorT>
  static bool sample(const AccessorT &accessor,
                     const openvdb::Vec3R &coord,
                     typename AccessorT::ValueType &result)
  {
    return grid_sampling::sample_tree<KernelT>(accessor, coord, result);
  }

  template<class AccessorT>
  static typename AccessorT::ValueType sample(const AccessorT &accessor,
                                              const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree<KernelT>(accessor, coord);
  }
};

}  // namespace grid_sampling

/**
 * Grid value sampler using nearest-point kernels.
 */
using NearestPointSampler = grid_sampling::SamplerWithKernel<grid_sampling::NearestPointKernel>;

/**
 * Grid value sampler using linear kernels.
 */
using LinearSampler = grid_sampling::SamplerWithKernel<grid_sampling::LinearKernel>;

/**
 * Grid value sampler using quadratic B-spline kernels.
 */
using QuadraticBSplineSampler =
    grid_sampling::SamplerWithKernel<grid_sampling::QuadraticBSplineKernel>;

/**
 * Grid value sampler using cubic B-spline kernels.
 */
using CubicBSplineSampler = grid_sampling::SamplerWithKernel<grid_sampling::CubicBSplineKernel>;

#endif

}  // namespace blender::geometry
