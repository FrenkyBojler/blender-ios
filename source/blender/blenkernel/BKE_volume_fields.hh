/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_math_matrix_types.hh"

#include "BKE_volume_openvdb.hh"

#include "FN_field.hh"

namespace blender::bke {

#ifdef WITH_OPENVDB

/* Index space of a single leaf buffer. */
class GridLeafNodeFieldContext : public fn::FieldContext {
  /* Node type used to define leaf buffer size. */
  using TopologyNodeType = openvdb::FloatTree::LeafNodeType;

  /* Base-2 exponent of the leaf dimension. */
  static const int32_t LOG2DIM = TopologyNodeType::LOG2DIM;
  /* Leaf buffer dimension along one coordinate axis. */
  static const int32_t DIM = 1 << LOG2DIM;
  /* Total number of voxels in a leaf buffer. */
  static const int32_t NUM_VOXELS = 1 << (3 * LOG2DIM);

 private:
  float4x4 transform_;
  int3 leaf_origin_;

 public:
  GridLeafNodeFieldContext(const float4x4 &transform, const int3 &leaf_origin)
      : transform_(transform), leaf_origin_(leaf_origin)
  {
  }

  const float4x4 &transform() const
  {
    return transform_;
  }

  const int3 &leaf_origin() const
  {
    return leaf_origin_;
  }

  int64_t size() const
  {
    return NUM_VOXELS;
  }

  static int3 index_to_local_coord(int64_t index)
  {
    BLI_assert(index < NUM_VOXELS);
    int3 xyz;
    xyz.x = index >> 2 * LOG2DIM;
    index &= ((1 << 2 * LOG2DIM) - 1);
    xyz.y = index >> LOG2DIM;
    xyz.z = index & ((1 << LOG2DIM) - 1);
    return xyz;
  }

  static int64_t local_coord_to_index(const int3 &xyz)
  {
    BLI_assert((xyz.x & (DIM - 1u)) < DIM && (xyz.y & (DIM - 1u)) < DIM &&
               (xyz.z & (DIM - 1u)) < DIM);
    return ((xyz.x & (DIM - 1u)) << (2 * LOG2DIM)) + ((xyz.y & (DIM - 1u)) << LOG2DIM) +
           (xyz.z & (DIM - 1u));
  }

  int3 index_to_global_coord(const int64_t index) const
  {
    return index_to_local_coord(index) + leaf_origin_;
  }

  int64_t global_coord_to_index(const int3 &coord) const
  {
    return local_coord_to_index(coord - leaf_origin_);
  }
};

VArray<float3> voxel_coordinate_varray(const GridLeafNodeFieldContext &context);
VArray<float3> voxel_center_varray(const GridLeafNodeFieldContext &context);

#endif

}  // namespace blender::bke
