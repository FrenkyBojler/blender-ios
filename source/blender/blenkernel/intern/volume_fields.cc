/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"

#include "BKE_volume_fields.hh"

namespace blender::bke {

#ifdef WITH_OPENVDB

VArray<float3> voxel_coordinate_varray(const GridLeafNodeFieldContext &context)
{
  return VArray<float3>::ForFunc(context.size(), [context](const int64_t index) {
    const int3 coord = context.index_to_global_coord(index);
    return float3(coord);
  });
}

VArray<float3> voxel_center_varray(const GridLeafNodeFieldContext &context)
{
  const float4x4 transform = context.transform();
  return VArray<float3>::ForFunc(context.size(), [context, transform](const int64_t index) {
    const int3 coord = context.index_to_global_coord(index);
    /* Offset by 0.5 to get the center position. */
    return math::transform_point(transform, float3(coord) + float3(0.5f));
  });
}

#endif

}  // namespace blender::bke
