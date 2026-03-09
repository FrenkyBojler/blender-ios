/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_generic_span.hh"
#include "BLI_math_base.hh"
#include "BLI_string_ref.hh"

#include "GEO_grid_samplers.hh"

#include "BKE_attribute.hh"
#include "BKE_volume_enums.hh"
#include "BKE_volume_grid.hh"

namespace blender {

struct Volume;

/** \file
 * \ingroup geo
 */

namespace geometry {

#ifdef WITH_OPENVDB

/**
 * Add a new fog VolumeGrid to the Volume by converting the supplied points.
 */
bke::VolumeGridData *fog_volume_grid_add_from_points(Volume *volume,
                                                     StringRefNull name,
                                                     Span<float3> positions,
                                                     Span<float> radii,
                                                     float voxel_size,
                                                     float density);

bke::VolumeGrid<float> points_to_sdf_grid(Span<float3> positions,
                                          Span<float> radii,
                                          float voxel_size);

/** True for data types that can be stored as point data grid attributes. */
bool is_point_attribute_grid_supported(const CPPType &cpp_type);

/**
 * Description of an attribute array to store in a point data grid.
 */
struct PointDataGridAttributeInfo {
  StringRef name;
  GSpan data;
};

using PointAttributeNameMap = Vector<std::pair<std::string, std::string>>;

struct MappedPointDataGrid {
  /* Point data grid. */
  bke::GVolumeGrid grid;
  /* Attribute pairs mapping original attribute names to internal identifiers.
   * OpenVDB PointDataGrid does not allow certain characters in attribute names, so internal names
   * are generated and mapped to original names. */
  PointAttributeNameMap attribute_map;
};

/**
 * Construct a point data grid from a positions array and optional attributes.
 * The resulting grid is of the \a VOLUME_GRID_POINTS type.
 *
 * \param positions Point positions array.
 * \param attributes Attributes to store in the grid.
 * \param transform Grid transform defining voxel size and offset.
 */
MappedPointDataGrid points_to_point_data_grid(const Span<float3> positions,
                                              const Span<PointDataGridAttributeInfo> attributes,
                                              const float4x4 &transform);

/**
 * Construct a point data grid from a positions array and optional attributes.
 * The resulting grid is of the \a VOLUME_GRID_POINTS type.
 *
 * \param positions Point positions array.
 * \param attributes Attributes to store in the grid.
 * \param attribute_filter Optional filter for attributes to be stored.
 * \param transform Grid transform defining voxel size and offset.
 */
MappedPointDataGrid points_to_point_data_grid(const VArray<float3> positions,
                                              const bke::AttributeAccessor &attributes,
                                              const bke::AttributeFilter &attribute_filter,
                                              const float4x4 &transform);

struct PointRasterizeAttributeInfo {
  StringRef name;
  const CPPType &type;
  bool use_staggered_vector;
  bool use_affine_vector;
  bool use_divergence;
};

/* TODO For ultimate flexibility a multi-function based kernel transfer class could be implemented,
 * but will require modifying the OpenVDB rasterization function to avoid overhead and remain
 * efficient. For most purposes adding a fixed kernel type here is sufficient. */
enum class KernelType {
  /* Constant weight in each voxel. */
  Constant,
  /* Linear falloff over the voxel range. */
  Linear,
  /* Quadratic b-spline kernel (see "Drucker-Prager Elastoplasticity for Sand Animation"). */
  QuadraticBSpline,
  /* Cubic b-spline kernel (see "Drucker-Prager Elastoplasticity for Sand Animation"). */
  CubicBSpline,
};

namespace kernel_functions {

inline int kernel_size(const KernelType kernel_type)
{
  switch (kernel_type) {
    case KernelType::Constant:
      return geometry::grid_sampling::ConstantKernel::size;
    case KernelType::Linear:
      return geometry::grid_sampling::LinearKernel::size;
    case KernelType::QuadraticBSpline:
      return geometry::grid_sampling::QuadraticBSplineKernel::size;
    case KernelType::CubicBSpline:
      return geometry::grid_sampling::CubicBSplineKernel::size;
  }
  BLI_assert_unreachable();
  return 0;
}

inline float kernel_eval_component(const KernelType kernel_type, const float t)
{
  switch (kernel_type) {
    case KernelType::Constant:
      return geometry::grid_sampling::ConstantKernel::weight(t);
    case KernelType::Linear:
      return geometry::grid_sampling::LinearKernel::weight(t);
    case KernelType::QuadraticBSpline:
      return geometry::grid_sampling::QuadraticBSplineKernel::weight(t);
    case KernelType::CubicBSpline:
      return geometry::grid_sampling::CubicBSplineKernel::weight(t);
  }
  return 0.0f;
}

inline float kernel_gradient_eval_component(const KernelType kernel_type, const float t)
{
  switch (kernel_type) {
    case KernelType::Constant:
      return geometry::grid_sampling::ConstantKernel::derivative(t);
    case KernelType::Linear:
      return geometry::grid_sampling::LinearKernel::derivative(t);
    case KernelType::QuadraticBSpline:
      return geometry::grid_sampling::QuadraticBSplineKernel::derivative(t);
    case KernelType::CubicBSpline:
      return geometry::grid_sampling::CubicBSplineKernel::derivative(t);
  }
  return 0.0f;
}

inline float kernel_eval(const KernelType kernel_type, const float3 &v)
{
  return kernel_eval_component(kernel_type, v.x) * kernel_eval_component(kernel_type, v.y) *
         kernel_eval_component(kernel_type, v.z);
}

inline float3 kernel_gradient_eval(const KernelType kernel_type, const float3 &v)
{
  const float vx = kernel_eval_component(kernel_type, v.x);
  const float vy = kernel_eval_component(kernel_type, v.y);
  const float vz = kernel_eval_component(kernel_type, v.z);
  return {kernel_gradient_eval_component(kernel_type, v.x) * vy * vz,
          vx * kernel_gradient_eval_component(kernel_type, v.y) * vz,
          vx * vy * kernel_gradient_eval_component(kernel_type, v.z)};
}

}  // namespace kernel_functions

/**
 * Rasterize points into grids using a custom weighting kernel.
 * Each point attribute generates an output grid.
 * Each voxel contains the weighted sum of points within the maximum range of the voxel center.
 * Each point contributes a value according to the kernel function. The kernel function takes
 * the distance between voxel center and particle and computes a weighting factor, which should
 * fall off to zero within the maximum distance. The maximum distance is a multiple of the point
 * data grid voxel size.
 *
 * \param point_data_grid Point grid with optional attributes.
 * \param kernel_fn Weighting kernel function, takes a \a float3 distance vector and outputs float.
 * \param transform Grid transform defining voxel size and offset.
 * \param max_voxel_range Maximum range of point data grid voxels contributing to a target voxel.
 */
void points_rasterize(const MappedPointDataGrid &point_data_grid,
                      const KernelType kernel_type,
                      Span<PointRasterizeAttributeInfo> point_attributes,
                      const float4x4 &transform,
                      MutableSpan<bke::GVolumeGrid> r_attribute_grids);

#endif
}  // namespace geometry
}  // namespace blender
