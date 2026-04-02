/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#ifdef WITH_OPENVDB

#  include <openvdb/openvdb.h>              /* IWYU pragma: export */
#  include <openvdb/points/PointDataGrid.h> /* IWYU pragma: export */
#  include <optional>

#  include "BLI_bounds_types.hh"
#  include "BLI_math_matrix_types.hh"
#  include "BLI_math_vector_types.hh"
#  include "BLI_parameter_pack_utils.hh"
#  include "BLI_string_ref.hh"

#  include "BKE_volume_enums.hh"
#  include "BKE_volume_grid_fwd.hh"

#  include "openvdb_fwd.hh"

namespace blender {

struct Volume;

bke::VolumeGridData *BKE_volume_grid_add_vdb(Volume &volume,
                                             StringRef name,
                                             openvdb::GridBase::Ptr vdb_grid);

void BKE_volume_metadata_set(Volume &volume, openvdb::MetaMap::Ptr metadata);

std::optional<Bounds<float3>> BKE_volume_grid_bounds(openvdb::GridBase::ConstPtr grid);

/**
 * Return a new grid pointer with only the metadata and transform changed.
 * This is useful for instances, where there is a separate transform on top of the original
 * grid transform that must be applied for some operations that only take a grid argument.
 */
openvdb::GridBase::ConstPtr BKE_volume_grid_shallow_transform(openvdb::GridBase::ConstPtr grid,
                                                              const float4x4 &transform);

float4x4 BKE_volume_transform_to_blender(const openvdb::math::Transform &transform);
openvdb::math::Transform BKE_volume_transform_to_openvdb(const float4x4 &transform);

template<typename OpType>
auto BKE_volume_grid_type_operation(const VolumeGridType grid_type, OpType &&op)
{
  switch (grid_type) {
    case VolumeGridFloat:
      return op.template operator()<openvdb::FloatGrid>();
    case VolumeGridVectorFloat:
      return op.template operator()<openvdb::Vec3fGrid>();
    case VolumeGridBoolean:
      return op.template operator()<openvdb::BoolGrid>();
    case VolumeGridDouble:
      return op.template operator()<openvdb::DoubleGrid>();
    case VolumeGridInt:
      return op.template operator()<openvdb::Int32Grid>();
    case VolumeGridInT64:
      return op.template operator()<openvdb::Int64Grid>();
    case VolumeGridVectorInt:
      return op.template operator()<openvdb::Vec3IGrid>();
    case VolumeGridVectorDouble:
      return op.template operator()<openvdb::Vec3dGrid>();
    case VolumeGridMask:
      return op.template operator()<openvdb::MaskGrid>();
    case VolumeGridPoints:
      return op.template operator()<openvdb::points::PointDataGrid>();
    case VolumeGridUnknown:
      break;
  }

  /* Should never be called. */
  BLI_assert_msg(0, "should never be reached");
  return op.template operator()<openvdb::FloatGrid>();
}

template<typename Fn>
inline void BKE_volume_grid_type_to_static_type(const VolumeGridType grid_type, Fn &&fn)
{
  switch (grid_type) {
    case VolumeGridFloat:
      return fn.template operator()<openvdb::FloatGrid>();
    case VolumeGridVectorFloat:
      return fn.template operator()<openvdb::Vec3fGrid>();
    case VolumeGridBoolean:
      return fn.template operator()<openvdb::BoolGrid>();
    case VolumeGridDouble:
      return fn.template operator()<openvdb::DoubleGrid>();
    case VolumeGridInt:
      return fn.template operator()<openvdb::Int32Grid>();
    case VolumeGridInT64:
      return fn.template operator()<openvdb::Int64Grid>();
    case VolumeGridVectorInt:
      return fn.template operator()<openvdb::Vec3IGrid>();
    case VolumeGridVectorDouble:
      return fn.template operator()<openvdb::Vec3dGrid>();
    case VolumeGridMask:
      return fn.template operator()<openvdb::MaskGrid>();
    case VolumeGridPoints:
      return fn.template operator()<openvdb::points::PointDataGrid>();
    case VolumeGridUnknown:
      break;
  }
  BLI_assert_unreachable();
}

template<typename Fn>
inline bool BKE_volume_grid_type_to_blender_value_type(const VolumeGridType grid_type, Fn &&fn)
{
  switch (grid_type) {
    case VolumeGridFloat:
      fn.template operator()<float>();
      return true;
    case VolumeGridVectorFloat:
      fn.template operator()<float3>();
      return true;
    case VolumeGridBoolean:
      fn.template operator()<bool>();
      return true;
    case VolumeGridDouble:
      fn.template operator()<double>();
      return true;
    case VolumeGridInt:
      fn.template operator()<int>();
      return true;
    case VolumeGridInT64:
      fn.template operator()<int64_t>();
      return true;
    case VolumeGridVectorInt:
      fn.template operator()<int3>();
      return true;
    case VolumeGridVectorDouble:
      fn.template operator()<double3>();
      return true;
    case VolumeGridMask:
    case VolumeGridPoints:
    case VolumeGridUnknown:
      break;
  }
  return false;
}

openvdb::GridBase::Ptr BKE_volume_grid_create_with_changed_resolution(
    const VolumeGridType grid_type, const openvdb::GridBase &old_grid, float resolution_factor);

}  // namespace blender

#endif
