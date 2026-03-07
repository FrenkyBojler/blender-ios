/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

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

template<typename T> struct GradientTypeTraits;
template<> struct GradientTypeTraits<float> {
  using type = float3;
};
template<> struct GradientTypeTraits<float3> {
  using type = float3x3;
};

template<typename T> struct OpenvdbGradientTypeTraits;
template<> struct OpenvdbGradientTypeTraits<float> {
  using type = openvdb::Vec3s;
};
template<> struct OpenvdbGradientTypeTraits<openvdb::Vec3s> {
  using type = openvdb::Mat3s;
};

template<typename T> using GradientType = typename GradientTypeTraits<T>::type;
template<typename T> using OpenvdbGradientType = typename OpenvdbGradientTypeTraits<T>::type;

template<int N, class TreeT>
bool probe_values(const TreeT &tree,
                  const openvdb::Vec3i &index,
                  typename TreeT::ValueType (&data)[N][N][N])
{
  constexpr int start_offset = ((N + 1) >> 1) - 1;
  const openvdb::Vec3i start_index = index -
                                     openvdb::Vec3i(start_offset, start_offset, start_offset);

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  bool active = false;
  for (int dx = 0, ix = start_index.x(); dx < N; ++dx, ++ix) {
    for (int dy = 0, iy = start_index.y(); dy < N; ++dy, ++iy) {
      for (int dz = 0, iz = start_index.z(); dz < N; ++dz, ++iz) {
        if (tree.probeValue(openvdb::Coord(ix, iy, iz), data[dx][dy][dz])) {
          active = true;
        }
      }
    }
  }
  return active;
}

template<int N, class TreeT>
void get_values(const TreeT &tree,
                const openvdb::Vec3i &index,
                typename TreeT::ValueType (&data)[N][N][N])
{
  constexpr int start_offset = ((N + 1) >> 1) - 1;
  const openvdb::Vec3i start_index = index -
                                     openvdb::Vec3i(start_offset, start_offset, start_offset);

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  for (int dx = 0, ix = start_index.x(); dx < N; ++dx, ++ix) {
    for (int dy = 0, iy = start_index.y(); dy < N; ++dy, ++iy) {
      for (int dz = 0, iz = start_index.z(); dz < N; ++dz, ++iz) {
        data[dx][dy][dz] = tree.getValue(openvdb::Coord(ix, iy, iz));
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
  for (int dx = 0; dx < N; ++dx) {
    ValueT vy[N];
    for (int dy = 0; dy < N; ++dy) {
      const ValueT *vz = &data[dx][dy][0];
      vy[dy] = kernel_fn(vz, uvw.z());
    }
    vx[dx] = kernel_fn(vy, uvw.y());
  }
  result = kernel_fn(vx, uvw.x());
}

/* Interpolate a gradient in 3D using a function that combines a 1-dimensional array and its
 * derivative function. */
template<typename ValueT, int N, typename KernelFn, typename DerivativeFn>
void interpolate_gradient_3d(ValueT (&data)[N][N][N],
                             const openvdb::Vec3R &uvw,
                             KernelFn kernel_fn,
                             DerivativeFn derivative_fn,
                             OpenvdbGradientType<ValueT> &result)
{
  using GradientT = OpenvdbGradientType<ValueT>;

  ValueT vvx[N], vgx[N], gvx[N];
  for (int dx = 0; dx < N; ++dx) {
    ValueT vy[N], gy[N];
    for (int dy = 0; dy < N; ++dy) {
      const ValueT *vz = &data[dx][dy][0];
      vy[dy] = kernel_fn(vz, uvw.z());
      gy[dy] = derivative_fn(vz, uvw.z());
    }
    vvx[dx] = kernel_fn(vy, uvw.y());
    vgx[dx] = kernel_fn(gy, uvw.y());
    gvx[dx] = derivative_fn(vy, uvw.y());
  }
  result = GradientT(
      derivative_fn(vvx, uvw.x()), kernel_fn(gvx, uvw.x()), kernel_fn(vgx, uvw.x()));
}

template<typename Kernel, class TreeT>
bool sample_tree(const TreeT &tree, const openvdb::Vec3R &coord, typename TreeT::ValueType &result)
{
  using ValueT = typename TreeT::ValueType;

  const openvdb::Vec3i index = openvdb::tools::local_util::floorVec3(coord);
  const openvdb::Vec3R uvw = coord - index;

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  constexpr int N = Kernel::size;
  ValueT data[N][N][N];
  bool active = probe_values(tree, index, data);
  interpolate_value_3d(data, uvw, Kernel::template weight<ValueT>, result);

  return active;
}

template<typename Kernel, class TreeT>
typename TreeT::ValueType sample_tree(const TreeT &tree, const openvdb::Vec3R &coord)
{
  using ValueT = typename TreeT::ValueType;

  const openvdb::Vec3i index = openvdb::tools::local_util::floorVec3(coord);
  const openvdb::Vec3R uvw = coord - index;

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  constexpr int N = Kernel::size;
  ValueT data[N][N][N];
  get_values(tree, index, data);

  ValueT result;
  interpolate_value_3d(data, uvw, Kernel::template weight<ValueT>, result);
  return result;
}

template<typename Kernel, class TreeT>
bool sample_tree_gradient(const TreeT &tree,
                          const openvdb::Vec3R &coord,
                          OpenvdbGradientType<typename TreeT::ValueType> &result)
{
  using ValueT = typename TreeT::ValueType;

  const openvdb::Vec3i index = openvdb::tools::local_util::floorVec3(coord);
  const openvdb::Vec3R uvw = coord - index;

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  constexpr int N = Kernel::size;
  ValueT data[N][N][N];
  bool active = probe_values(tree, index, data);
  interpolate_gradient_3d(
      data, uvw, Kernel::template weight<ValueT>, Kernel::template derivative<ValueT>, result);

  return active;
}

template<typename Kernel, class TreeT>
OpenvdbGradientType<typename TreeT::ValueType> sample_tree_gradient(const TreeT &tree,
                                                                    const openvdb::Vec3R &coord)
{
  using ValueT = typename TreeT::ValueType;

  const openvdb::Vec3i index = openvdb::tools::local_util::floorVec3(coord);
  const openvdb::Vec3R uvw = coord - index;

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  constexpr int N = Kernel::size;
  ValueT data[N][N][N];
  get_values(tree, index, data);

  ValueT result;
  interpolate_gradient_3d(
      data, uvw, Kernel::template weight<ValueT>, Kernel::template derivative<ValueT>, result);
  return result;
}

/**
 * Nearest-neighbor kernel function with a nominal (zero) gradient output.
 */
struct ConstantKernel {
  static constexpr float range = 0.5f;
  static constexpr int size = 1;

  static float weight(float x)
  {
    if (x < -0.5f) {
      return 0.0f;
    }
    if (x < 0.5f) {
      return 1.0f;
    }
    return 0.0f;
  }

  static float derivative(float /*x*/)
  {
    return 0.0f;
  }

  template<class ValueT> static ValueT weight(const ValueT *value, double /*weight*/)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    return value[0];
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT> static ValueT derivative(const ValueT * /*value*/, double /*weight*/)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    return ValueT(0.0);
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }
};

/**
 * Linear kernel function that also supports gradient output.
 */
struct LinearKernel {
  static constexpr float range = 1.0f;
  static constexpr int size = 2;

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

  template<class ValueT> static ValueT weight(const ValueT *value, double weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    const ValueT lin = value[1] - value[0];
    const ValueT con = value[0];
    return weight * lin + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT> static ValueT derivative(const ValueT *value, double weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    const ValueT con = double(weight != 0.0) * (value[1] - value[0]);
    return con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }
};

/**
 * Quadratic B-spline kernel function as described in
 * Steffen et al., "Analysis and reduction of quadrature errors in the material point method (MPM)"
 *
 * The kernel is a piece-wise quadratic spline with two parts:
 * f(x) = -|x|^2 + 3/4               for   0 <= |x| < 1/2
 * f(x) = 1/2*|x|^2 - 3/2*|x| + 9/8  for 1/2 <= |x| < 3/2
 * f(x) = 0                          for 3/2 <= |x|
 *
 * The derivative is a piece-wise linear function:
 * f(x) = -2*|x|                     for   0 <= |x| < 1/2
 * f(x) = |x| - 3/2                  for 1/2 <= |x| < 3/2
 * f(x) = 0                          for 3/2 <= |x|
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
  static constexpr float range = 1.5f;
  static constexpr int size = 4;

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
      return -x - 1.5f;
    }
    if (x < 0.0f) {
      return 2.0f * x;
    }
    if (x < 0.5f) {
      return -2.0f * x;
    }
    if (x < 1.5f) {
      return x - 1.5f;
    }
    return 0.0f;
  }

  template<class ValueT> static ValueT weight(const ValueT *value, double weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    if (weight < 0.5) {
      const ValueT sqr = 0.5 * (value[0] + value[2]) - value[1];
      const ValueT lin = 0.5 * (value[2] - value[0]);
      const ValueT con = 0.125 * (value[0] + value[2]) + 0.75 * value[1];
      return weight * (weight * sqr + lin) + con;
    }

    const ValueT sqr = 0.5 * (value[1] + value[3]) - value[2];
    const ValueT lin = -1.5 * value[1] + 2 * value[2] - 0.5 * value[3];
    const ValueT con = 1.125 * value[1] - 0.25 * value[2] + 0.125 * value[3];
    return weight * (weight * sqr + lin) + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT> static ValueT derivative(const ValueT *value, double weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    if (weight < 0.5) {
      const ValueT lin = value[0] - 2.0 * value[1] + value[2];
      const ValueT con = 0.5 * (value[2] - value[0]);
      return weight * lin + con;
    }

    const ValueT lin = value[1] - 2.0 * value[2] + value[3];
    const ValueT con = -1.5 * value[1] + 2.0 * value[2] - 0.5 * value[3];
    return weight * lin + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }
};

/**
 * Cubic B-spline kernel function as described in
 * Steffen et al., "Analysis and reduction of quadrature errors in the material point method (MPM)"
 *
 * The kernel is a piece-wise cubic spline with two parts:
 * f(x) = 1/2*|x|^3 - |x|^2 + 2/3           for 0 <= |x| < 1
 * f(x) = -1/6*|x|^3 + |x|^2 - 2*|x| + 4/3  for 1 <= |x| < 2
 * f(x) = 0                                 for 2 <= |x|
 *
 * The derivative is a piece-wise quadratic function:
 * f(x) = 3/2*|x|^2 - 2*|x|                 for 0 <= |x| < 1
 * f(x) = -1/2*|x|^2 + 2*|x| - 2            for 1 <= |x| < 2
 * f(x) = 0                                 for 2 <= |x|
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
  static constexpr float range = 2.0f;
  static constexpr int size = 4;

  static float weight(float x)
  {
    if (x < -2.0f) {
      return 0.0f;
    }
    if (x < -1.0f) {
      return ((x / 6.0f + 1.0f) * x + 2.0f) * x + 4.0f / 3.0f;
    }
    if (x < 0.0f) {
      return (-x - 1.0f) * x * x + 2.0f / 3.0f;
    }
    if (x < 1.0f) {
      return (x - 1.0f) * x * x + 2.0f / 3.0f;
    }
    if (x < 2.0f) {
      return ((-x / 6.0f + 1.0f) * x - 2.0f) * x + 4.0f / 3.0f;
    }
    return 0.0f;
  }

  static float derivative(float x)
  {
    if (x < -1.5f) {
      return 0.0f;
    }
    if (x < -0.5f) {
      return -x - 1.5f;
    }
    if (x < 0.0f) {
      return 2.0f * x;
    }
    if (x < 0.5f) {
      return -2.0f * x;
    }
    if (x < 1.5f) {
      return x - 1.5f;
    }
    return 0.0f;
  }

  template<class ValueT> static ValueT weight(const ValueT *value, double weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    constexpr double inv6 = 1.0 / 6.0;
    const ValueT cub = inv6 * (value[3] - value[0]) + 0.5 * (value[1] - value[2]);
    const ValueT sqr = 0.5 * (value[0] + value[2]) - value[1];
    const ValueT lin = 0.5 * (value[2] - value[0]);
    const ValueT con = inv6 * (value[0] + 4.0 * value[1] + value[2]);
    return weight * (weight * (weight * cub + sqr) + lin) + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT> static ValueT derivative(const ValueT *value, double weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    const ValueT sqr = 0.5 * (value[3] - value[0]) + 1.5 * (value[1] - value[2]);
    const ValueT lin = value[0] - 2.0 * value[1] + value[2];
    const ValueT con = 0.5 * (value[2] - value[0]);
    return weight * (weight * sqr + lin) + con;
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }
};

}  // namespace grid_sampling

/**
 * Grid value sampler using linear kernels.
 */
struct LinearSampler {
  template<class TreeT>
  static bool sample(const TreeT &tree,
                     const openvdb::Vec3R &coord,
                     typename TreeT::ValueType &result)
  {
    return grid_sampling::sample_tree<grid_sampling::LinearKernel>(tree, coord, result);
  }

  template<class TreeT>
  static typename TreeT::ValueType sample(const TreeT &tree, const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree<grid_sampling::LinearKernel>(tree, coord);
  }

  template<class TreeT>
  static bool sample_gradient(
      const TreeT &tree,
      const openvdb::Vec3R &coord,
      grid_sampling::OpenvdbGradientType<typename TreeT::ValueType> &result)
  {
    return grid_sampling::sample_tree_gradient<grid_sampling::LinearKernel>(tree, coord, result);
  }

  template<class TreeT>
  static grid_sampling::OpenvdbGradientType<typename TreeT::ValueType> sample_gradient(
      const TreeT &tree, const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree_gradient<grid_sampling::LinearKernel>(tree, coord);
  }
};

/**
 * Grid value sampler using quadratic B-spline kernels.
 */
struct QuadraticBSplineSampler {
  template<class TreeT>
  static bool sample(const TreeT &tree,
                     const openvdb::Vec3R &coord,
                     typename TreeT::ValueType &result)
  {
    return grid_sampling::sample_tree<grid_sampling::QuadraticBSplineKernel>(tree, coord, result);
  }

  template<class TreeT>
  static typename TreeT::ValueType sample(const TreeT &tree, const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree<grid_sampling::QuadraticBSplineKernel>(tree, coord);
  }

  template<class TreeT>
  static bool sample_gradient(
      const TreeT &tree,
      const openvdb::Vec3R &coord,
      grid_sampling::OpenvdbGradientType<typename TreeT::ValueType> &result)
  {
    return grid_sampling::sample_tree_gradient<grid_sampling::QuadraticBSplineKernel>(
        tree, coord, result);
  }

  template<class TreeT>
  static grid_sampling::OpenvdbGradientType<typename TreeT::ValueType> sample_gradient(
      const TreeT &tree, const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree_gradient<grid_sampling::QuadraticBSplineKernel>(tree, coord);
  }
};

/**
 * Grid value sampler using cubic B-spline kernels.
 */
struct CubicBSplineSampler {
  template<class TreeT>
  static bool sample(const TreeT &tree,
                     const openvdb::Vec3R &coord,
                     typename TreeT::ValueType &result)
  {
    return grid_sampling::sample_tree<grid_sampling::CubicBSplineKernel>(tree, coord, result);
  }

  template<class TreeT>
  static typename TreeT::ValueType sample(const TreeT &tree, const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree<grid_sampling::CubicBSplineKernel>(tree, coord);
  }

  template<class TreeT>
  static bool sample_gradient(
      const TreeT &tree,
      const openvdb::Vec3R &coord,
      grid_sampling::OpenvdbGradientType<typename TreeT::ValueType> &result)
  {
    return grid_sampling::sample_tree_gradient<grid_sampling::CubicBSplineKernel>(
        tree, coord, result);
  }

  template<class TreeT>
  static grid_sampling::OpenvdbGradientType<typename TreeT::ValueType> sample_gradient(
      const TreeT &tree, const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree_gradient<grid_sampling::CubicBSplineKernel>(tree, coord);
  }
};

#endif

}  // namespace blender::geometry
