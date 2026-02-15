/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_array.hh"
#include "BLI_mutex.hh"

#include "GEO_xpbd_geometry_ref.hh"

namespace blender::xpbd {

/**
 * Updater that writes that accumulates all changes into a separate array. This is
 * non-deterministic because float addition is not commutative. It mainly exists for testing
 * purposes.
 */
class NonDeterministicJacobianUpdater {
 public:
  struct OffsetItem {
    Mutex linear_mutex;
    int linear_counter = 0;
    float3 linear_offset = float3(0.0f);
    Mutex rotation_mutex;
    int rotation_counter = 0;
    float4 rotation_offset = float4(0.0f);
  };

  struct GeometryItem {
    Array<OffsetItem> offsets;
    IndexRange range;
  };

 private:
  Span<GeometryRef> geometry_refs_;
  Array<GeometryItem> items_;

 public:
  NonDeterministicJacobianUpdater(Span<GeometryRef> geometry_refs);
  NonDeterministicJacobianUpdater(Span<GeometryRef> geometry_refs,
                                  const int geo_i,
                                  const IndexRange range);

  void update_position(const int geo_i, const int point_i, const float3 &offset)
  {
    GeometryItem &geo_item = items_[geo_i];
    OffsetItem &item = geo_item.offsets[point_i - geo_item.range.start()];
    std::lock_guard lock(item.linear_mutex);
    item.linear_counter++;
    item.linear_offset += offset;
  }

  void update_rotation(const int geo_i, const int point_i, const math::Quaternion &offset)
  {
    GeometryItem &geo_item = items_[geo_i];
    OffsetItem &item = geo_item.offsets[point_i - geo_item.range.start()];
    std::lock_guard lock(item.rotation_mutex);
    item.rotation_counter++;
    item.rotation_offset += float4(offset);
  }

  void apply();
};

}  // namespace blender::xpbd
