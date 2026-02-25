/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include <cstdint>

namespace blender {

enum VolumeGridType : int8_t {
  VOLUME_GRID_UNKNOWN = 0,
  VOLUME_GRID_BOOLEAN,
  VOLUME_GRID_FLOAT,
  VOLUME_GRID_DOUBLE,
  VOLUME_GRID_INT,
  VOLUME_GRID_INT64,
  VOLUME_GRID_MASK,
  VOLUME_GRID_VECTOR_FLOAT,
  VOLUME_GRID_VECTOR_DOUBLE,
  VOLUME_GRID_VECTOR_INT,
  VOLUME_GRID_POINTS,
};

namespace bke {

enum class RasterizePointsWeighting {
  /* Sum of values in each voxel (multiplied by kernel factor). */
  Sum,
  /* Sum of values divided by total value in a voxel. */
  Average,
  /* Sum of values multiplied by mass and divided by total mass in a voxel. */
  WeightedAverage,
};

}

}  // namespace blender
