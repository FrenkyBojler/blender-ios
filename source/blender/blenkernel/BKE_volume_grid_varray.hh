/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#ifdef WITH_OPENVDB

#  include "BLI_math_vector_types.hh"
#  include "BLI_struct_equality_utils.hh"
#  include "BLI_vector_set.hh"
#  include "BLI_virtual_array_fwd.hh"

#  include "BKE_volume_openvdb.hh"

namespace blender::bke::volume_grid {

struct GridNodeOffset {
  openvdb::Index level;
  openvdb::Coord coord;
  int index_offset;

  BLI_STRUCT_EQUALITY_OPERATORS_2(GridNodeOffset, level, coord)

  uint64_t hash() const
  {
    return get_default_hash(this->level, this->coord);
  }
};

class GridNodeIndexMapping {
 private:
  VectorSet<GridNodeOffset> node_offsets_;

 public:
  int size() const;
  IndexRange index_range() const;
  template<typename NodeT> IndexRange get_node_range(const NodeT &node) const;
};

enum class GridValueOnOff { On, Off, Dense };

VArray<int3> varray_for_grid_min_coordinates(const VolumeGridData &grid,
                                             std::shared_ptr<GridNodeIndexMapping> index_mapping,
                                             const GridValueOnOff grid_value_filter);

}  // namespace blender::bke::volume_grid

namespace blender {
template<> struct DefaultHash<bke::volume_grid::GridNodeOffset> {
  uint64_t operator()(const bke::volume_grid::GridNodeOffset &node_offset) const
  {
    return value.GetHash();
  }
};

#endif

/** \} */
