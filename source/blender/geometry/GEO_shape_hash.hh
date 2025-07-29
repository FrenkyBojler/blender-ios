/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstdint>

#include "BLI_struct_equality_utils.hh"

namespace blender::bke {
class GeometrySet;
}

namespace blender::geometry::collision_shapes {

class GeometryShapeHash {
 private:
  uint64_t v1_ = 0;
  uint64_t v2_ = 0;

 public:
  GeometryShapeHash() = default;

  static GeometryShapeHash from_geometry(const bke::GeometrySet &geometry_set);

  uint64_t hash() const
  {
    return v1_;
  }

  BLI_STRUCT_EQUALITY_OPERATORS_2(GeometryShapeHash, v1_, v2_)
};

}  // namespace blender::geometry::collision_shapes
