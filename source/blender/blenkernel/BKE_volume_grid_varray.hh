/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#ifdef WITH_OPENVDB

#  include "BLI_generic_virtual_array.hh"
#  include "BLI_map.hh"
#  include "BLI_math_vector_types.hh"
#  include "BLI_struct_equality_utils.hh"
#  include "BLI_virtual_array_fwd.hh"

#  include "BKE_volume_grid.hh"

namespace blender::bke::volume_grid {

struct GridNodeKey {
  openvdb::Index level;
  openvdb::Coord coord;

  BLI_STRUCT_EQUALITY_OPERATORS_2(GridNodeKey, level, coord)

  uint64_t hash() const
  {
    return get_default_hash(level, coord);
  }
};

class GridNodeIndexMapping {
 private:
  GridIndexMappingParams params_;
  Map<GridNodeKey, IndexRange> node_ranges_;
  int64_t size_;

 public:
  static std::shared_ptr<GridNodeIndexMapping> from_grid(const VolumeGridData &grid,
                                                         const GridIndexMappingParams &params);

  const GridIndexMappingParams &params() const;

  int size() const;
  template<typename NodeT> IndexRange get_node_range(const NodeT &node) const;
};

VArray<int3> varray_for_grid_origin(const VolumeGridData &grid,
                                    const std::shared_ptr<GridNodeIndexMapping> &index_mapping);
VArray<int> varray_for_grid_level(const VolumeGridData &grid,
                                  const std::shared_ptr<GridNodeIndexMapping> &index_mapping);
VArray<int> varray_for_grid_size(const VolumeGridData &grid,
                                 const std::shared_ptr<GridNodeIndexMapping> &index_mapping);
VArray<bool> varray_for_grid_active(const VolumeGridData &grid,
                                    const std::shared_ptr<GridNodeIndexMapping> &index_mapping);
GVArray varray_for_grid_value(const VolumeGridData &grid,
                              const std::shared_ptr<GridNodeIndexMapping> &index_mapping);

}  // namespace blender::bke::volume_grid

#endif

/** \} */
